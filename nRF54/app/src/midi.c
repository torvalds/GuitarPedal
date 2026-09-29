/*
 * MIDI over Bluetooth Low Energy, both directions.
 *
 * The encoding is MMA/AMEI RP-052 ("Specification for MIDI over
 * Bluetooth Low Energy", 2015), which is Apple's protocol adopted as a
 * standard.  The format:
 *
 *	header byte	1 0 t t t t t t	  top 6 bits of the timestamp
 *	timestamp byte	1 t t t t t t t	  low 7 bits of the timestamp
 *	MIDI message	status + data, as on a wire
 *
 * A 13-bit millisecond timestamp, so it wraps every 8192 ms and the
 * receiver spots the wrap by timestampLow going *down*.
 *
 * Three rules are worth stating because they are what the code below is
 * shaped by, and getting any of them wrong produces a stream that some
 * hosts accept and others do not:
 *
 *  - Every packet starts with a header byte.  There is no exception.
 *  - Every MIDI status byte is preceded by a timestamp byte.
 *  - SysEx is the only message allowed to span packets, and a
 *    continuation packet is a header byte followed *straight* by data,
 *    with no timestamp byte.  That absence is the only thing telling
 *    the receiver it is a continuation rather than a new message.
 *
 * Running status - omitting a repeated status byte - is decoded here
 * because a host may send it, and is not generated: it saves one byte in
 * a packet of 244.
 *
 * Neither UUID is ours to choose: they are the ones every BLE MIDI host
 * already looks for.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "midi.h"

#define BT_UUID_MIDI_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x03b80e5a, 0xede8, 0x4b33, 0xa751, 0x6ce34ec4c700)
#define BT_UUID_MIDI_IO_VAL \
	BT_UUID_128_ENCODE(0x7772e5db, 0x3868, 0x4112, 0xa1a9, 0xf2669d106bf3)

static struct bt_uuid_128 midi_service_uuid =
	BT_UUID_INIT_128(BT_UUID_MIDI_SERVICE_VAL);
static struct bt_uuid_128 midi_io_uuid =
	BT_UUID_INIT_128(BT_UUID_MIDI_IO_VAL);

/*
 * The largest packet we will ever build.  The real limit is the
 * negotiated MTU less three, read per connection below; this is only
 * the buffer that has to be big enough for it.
 */
#define MIDI_BLE_MAX_PKT	(CONFIG_BT_L2CAP_TX_MTU - 3)
#define MIDI_BLE_MIN_PKT	20	/* the default MTU of 23, less three */

static struct bt_conn *midi_conn;

/* ------------------------------------------------------------------ */
/* How long a MIDI message is, from its status byte                    */
/* ------------------------------------------------------------------ */

static int midi_msg_len(uint8_t status)
{
	switch (status & 0xF0) {
	case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
		return 3;
	case 0xC0: case 0xD0:
		return 2;
	}

	switch (status) {
	case 0xF2:		/* song position */
		return 3;
	case 0xF1:		/* quarter frame */
	case 0xF3:		/* song select */
		return 2;
	}

	return 1;		/* system common and real-time with no data */
}

/* ------------------------------------------------------------------ */
/* Packing: MIDI messages out, BLE packets away                        */
/* ------------------------------------------------------------------ */

static struct {
	uint8_t buf[MIDI_BLE_MAX_PKT];
	uint16_t len;
	uint32_t sent, dropped;
} out;

/*
 * Packets handed to the stack and not yet on the air.
 *
 * bt_gatt_notify() returning 0 means queued, not sent, and the stack
 * holds only CONFIG_BT_ATT_TX_COUNT buffers.  A schema is about 150
 * packets, so sending as fast as the loop can build them fills those in
 * milliseconds and everything after is refused - and a packet dropped in
 * the middle of a SysEx costs the whole message, because the far end
 * waits for an F7 that was in the part thrown away.
 *
 * So the completion callback is the flow control: one slot per buffer
 * the stack has, taken before sending and given back when that packet is
 * actually gone.  Waiting for a slot is the back-pressure - it stops
 * this thread draining the UART, which deasserts RTS, which pauses the
 * pedal.  Every other stage of this link already works that way; this
 * was the one that did not.
 */
#define MIDI_BLE_SLOTS	CONFIG_BT_ATT_TX_COUNT

static struct {
	uint8_t buf[MIDI_BLE_MAX_PKT];
	struct bt_gatt_notify_params params;
} slot[MIDI_BLE_SLOTS];

static uint8_t slot_next;
static K_SEM_DEFINE(slot_free, MIDI_BLE_SLOTS, MIDI_BLE_SLOTS);

static void notify_done(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);

	k_sem_give(&slot_free);
}

static uint16_t midi_ble_now(void)
{
	return (uint16_t)(k_uptime_get_32() & 0x1FFF);
}

static uint16_t midi_ble_limit(void)
{
	uint16_t mtu;

	if (!midi_conn)
		return MIDI_BLE_MIN_PKT;

	mtu = bt_gatt_get_mtu(midi_conn);
	if (mtu < 23)
		return MIDI_BLE_MIN_PKT;
	if (mtu - 3 > MIDI_BLE_MAX_PKT)
		return MIDI_BLE_MAX_PKT;

	return mtu - 3;
}

/* Declared here, defined with the service below */
static const struct bt_gatt_attr *midi_value_attr(void);

/*
 * Is there room to take another byte?
 *
 * One byte may fill the packet being built and need it sent, and sending
 * needs one of the stack's buffers - so a free slot is the whole answer.
 * Asked by the loop before each byte, which is how the back-pressure
 * reaches the pedal instead of being waited out here.  The same shape as
 * nrf54_uart_ready() at the other end of the wire.
 */
bool midi_ble_ready(void)
{
	return k_sem_count_get(&slot_free) > 0;
}

void midi_ble_flush(void)
{
	int err;

	/*
	 * A header byte on its own carries nothing.  This is the ordinary
	 * case - the sender calls here whenever the UART goes quiet.
	 */
	if (out.len <= 1) {
		out.len = 0;
		return;
	}

	if (!midi_conn) {
		out.len = 0;
		return;
	}

	/*
	 * Never waits.  A caller that asked midi_ble_ready() first has a
	 * slot, so this cannot fire; 'noslot' reading non-zero means
	 * somebody added a caller that does not ask.
	 *
	 * Waiting here would pause the wrong thing - the loop rather than
	 * the pedal.  Declining to take bytes off the UART is what
	 * deasserts RTS.
	 */
	if (k_sem_take(&slot_free, K_NO_WAIT) != 0) {
		out.dropped++;
		out.len = 0;
		return;
	}

	memcpy(slot[slot_next].buf, out.buf, out.len);
	slot[slot_next].params = (struct bt_gatt_notify_params){
		.attr = midi_value_attr(),
		.data = slot[slot_next].buf,
		.len  = out.len,
		.func = notify_done,
	};

	err = bt_gatt_notify_cb(midi_conn, &slot[slot_next].params);
	if (err) {
		k_sem_give(&slot_free);
		out.dropped++;
	} else {
		slot_next = (slot_next + 1) % MIDI_BLE_SLOTS;
		out.sent++;
	}

	out.len = 0;
}

static void pkt_header(void)
{
	out.buf[0] = 0x80 | (midi_ble_now() >> 7);
	out.len = 1;
}

static void pkt_timestamp(void)
{
	out.buf[out.len++] = 0x80 | (midi_ble_now() & 0x7F);
}

/*
 * Room for 'need' more bytes, starting a fresh packet if there is not.
 *
 * It emits the header byte and nothing else, which is what makes the
 * SysEx continuation fall out rather than needing a case of its own:
 * pack_sysex_data() is the one caller that does not follow this with a
 * timestamp, so a packet it starts is a continuation - and the missing
 * timestamp is the only thing marking it as one.
 */
static void pkt_room(uint16_t need)
{
	if (out.len == 0)
		pkt_header();

	if (out.len + need <= midi_ble_limit())
		return;

	midi_ble_flush();
	pkt_header();
}

static void pack_message(const uint8_t *msg, int len)
{
	pkt_room(1 + len);
	pkt_timestamp();
	for (int i = 0; i < len; i++)
		out.buf[out.len++] = msg[i];
}

static void pack_sysex_start(void)
{
	pkt_room(2);
	pkt_timestamp();
	out.buf[out.len++] = 0xF0;
}

static void pack_sysex_data(uint8_t b)
{
	pkt_room(1);
	out.buf[out.len++] = b;
}

static void pack_sysex_end(void)
{
	pkt_room(2);
	pkt_timestamp();
	out.buf[out.len++] = 0xF7;
}

/* ------------------------------------------------------------------ */
/* Parsing: the UART's byte stream, into messages                      */
/* ------------------------------------------------------------------ */

static struct {
	uint8_t msg[3];
	int len;		/* bytes of 'msg' collected */
	int want;		/* how many the status byte asks for */
	uint8_t status;		/* running status */
	bool sysex;
} in;

void midi_ble_feed(uint8_t b)
{
	if (b >= 0xF8) {
		/*
		 * Real-time, which is legal anywhere at all - including
		 * between the status and data bytes of something else,
		 * and inside a SysEx.  So it is packed on its own and
		 * disturbs no state.
		 */
		pack_message(&b, 1);
		return;
	}

	if (b == 0xF0) {
		pack_sysex_start();
		in.sysex = true;
		in.want = 0;
		in.len = 0;
		in.status = 0;
		return;
	}

	if (b == 0xF7) {
		if (in.sysex) {
			pack_sysex_end();
			in.sysex = false;
		}
		return;
	}

	if (b >= 0x80) {
		/*
		 * A status byte while a SysEx is open means the SysEx
		 * was abandoned - the pedal reset, or a byte went
		 * missing.  Close it rather than leave the far end
		 * waiting: an unterminated SysEx makes every later
		 * message look like more of it.
		 */
		if (in.sysex) {
			pack_sysex_end();
			in.sysex = false;
		}

		in.status = b;
		in.msg[0] = b;
		in.len = 1;
		in.want = midi_msg_len(b);

		if (in.want == 1) {
			pack_message(in.msg, 1);
			in.len = 0;
			in.want = 0;
			in.status = 0;	/* system common cancels it */
		}
		return;
	}

	/* A data byte. */
	if (in.sysex) {
		pack_sysex_data(b);
		return;
	}

	if (in.want == 0) {
		/*
		 * Running status, or a byte with nothing in front of
		 * it.  The pedal sends whole messages, so the second is
		 * what arrives when the link has been resynchronised
		 * mid-message, and dropping it is right.
		 */
		if (!in.status)
			return;
		in.msg[0] = in.status;
		in.len = 1;
		in.want = midi_msg_len(in.status);
	}

	in.msg[in.len++] = b;
	if (in.len < in.want)
		return;

	pack_message(in.msg, in.len);
	in.len = 0;
	in.want = 0;
}

/* ------------------------------------------------------------------ */
/* Unpacking: BLE packets in, byte stream to the UART                  */
/* ------------------------------------------------------------------ */

static uint8_t dec_status;

static void midi_ble_decode(const uint8_t *buf, uint16_t len)
{
	uint16_t i = 1;		/* [0] is the header byte */

	if (len < 2 || !(buf[0] & 0x80))
		return;

	while (i < len) {
		if (!(buf[i] & 0x80)) {
			/*
			 * A data byte where a timestamp would be: this
			 * is SysEx continuation data, and its absence of
			 * a timestamp is exactly what says so.
			 */
			midi_uart_send(&buf[i], 1);
			i++;
			continue;
		}

		i++;		/* the timestamp byte itself carries no MIDI */
		if (i >= len)
			return;

		if (buf[i] & 0x80) {
			uint8_t status = buf[i++];

			midi_uart_send(&status, 1);
			if (status < 0xF0)
				dec_status = status;
			else if (status >= 0xF8)
				;	/* real-time cancels nothing */
			else
				dec_status = 0;
		} else if (dec_status) {
			/* Running status: the status byte was omitted. */
			midi_uart_send(&dec_status, 1);
		}

		while (i < len && !(buf[i] & 0x80))
			midi_uart_send(&buf[i++], 1);
	}
}

/* ------------------------------------------------------------------ */
/* The GATT service                                                    */
/* ------------------------------------------------------------------ */

/*
 * A read of the characteristic returns nothing, which is what the
 * specification asks for: there is no state to read, only a stream to be
 * notified of.
 */
static ssize_t midi_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	return 0;
}

static ssize_t midi_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	midi_ble_decode(buf, len);
	return len;
}

static void midi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("midi: notifications %s\n",
	       value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(midi_svc,
	BT_GATT_PRIMARY_SERVICE(&midi_service_uuid),
	BT_GATT_CHARACTERISTIC(&midi_io_uuid.uuid,
			       BT_GATT_CHRC_READ |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP |
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       midi_read, midi_write, NULL),
	BT_GATT_CCC(midi_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static const struct bt_gatt_attr *midi_value_attr(void)
{
	return &midi_svc.attrs[1];
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

/*
 * Flags, the name, and the service UUID: 3 + 7 + 18 bytes of the 31 an
 * advertisement has.  The UUID is in there because that is what makes a
 * scanner call this a MIDI device rather than an unknown one.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MIDI_SERVICE_VAL),
};

/*
 * Advertising is restarted from a work item rather than from the
 * disconnected callback it follows.
 *
 * That callback runs in the Bluetooth stack's own thread, where
 * bt_le_adv_start() can refuse with -EAGAIN because the stack is still
 * unwinding the connection it has just reported.  A refusal that is not
 * retried leaves the radio not advertising, and then nothing can find
 * it - so the error is printed rather than discarded.
 */
static void adv_start(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
				  NULL, 0);

	if (err)
		printk("bt: advertising failed, %d\n", err);
	else
		printk("bt: advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
}
static K_WORK_DEFINE(adv_work, adv_start);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("bt: connect failed, %u\n", err);
		return;
	}

	midi_conn = bt_conn_ref(conn);
	printk("bt: connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("bt: disconnected, %u\n", reason);

	if (midi_conn) {
		bt_conn_unref(midi_conn);
		midi_conn = NULL;
	}

	/* A part-built packet has nowhere to go now. */
	out.len = 0;

	/*
	 * Outstanding notifications go with the connection, and their
	 * callbacks may never run, so the slots are counted back by hand.
	 */
	k_sem_init(&slot_free, MIDI_BLE_SLOTS, MIDI_BLE_SLOTS);
	slot_next = 0;
	in.sysex = false;
	in.len = in.want = 0;
	in.status = dec_status = 0;

	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(midi_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

void midi_ble_start(void)
{
	int err = bt_enable(NULL);

	if (err) {
		printk("bt: enable failed, %d\n", err);
		return;
	}

	adv_start(NULL);
}
