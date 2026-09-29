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

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "midi.h"
#include "scan.h"
#include "bind.h"

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

	/*
	 * Why a packet did not go out, counted apart because the three
	 * have nothing to do with each other and only one of them is a
	 * Bluetooth problem: nobody was connected, the stack never gave a
	 * buffer back, or it refused the send.
	 */
	uint32_t noconn, noslot, failed;
	int err;		/* what it refused with, last time */

	uint32_t fed;		/* bytes in from the UART */
	uint32_t notified;	/* bytes handed to the stack */

	/*
	 * Whether anybody has asked to be sent anything, and how many
	 * times.  A client that believes it subscribed while this says it
	 * did not is the difference between a radio that will not send and
	 * a host that never asked - and the two look identical from the
	 * far end, which is why the count is here.
	 */
	uint16_t ccc;
	uint32_t ccc_n;

	/*
	 * Whether the pedal has been told what the line above says, and
	 * what it was told.  The change is noticed in a Bluetooth callback
	 * and sent from the main loop, because the transmit ring has one
	 * writer and that is the loop.
	 */
	bool told;
	bool listening;

	/* The pedal's pairing window, and a bond to report from the loop. */
	bool pairing;
	bool bonded_pending;
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
		out.noconn++;
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
		out.noslot++;
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
		out.failed++;
		out.err = err;
		out.dropped++;
	} else {
		slot_next = (slot_next + 1) % MIDI_BLE_SLOTS;
		out.sent++;
		out.notified += out.len;
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
/* The radio's own SysEx, which does not go on the air                 */
/* ------------------------------------------------------------------ */

/*
 * F0 7D 1x is the radio talking to the RP2354 rather than MIDI passing
 * through it.  The commands ride the same stream because there is only
 * one wire, and are taken out of it here so they never reach Bluetooth.
 *
 * Data bytes are seven bits, so an address goes out as twelve nibbles.
 */
#define RADIO_SYSEX_FIRST	0x10
#define RADIO_SYSEX_SCAN	0x10	/* in:  look for controllers */
#define RADIO_SYSEX_BIND	0x11	/* in:  use this one */
#define RADIO_SYSEX_FOUND	0x12	/* out: one that answered */
#define RADIO_SYSEX_DONE	0x13	/* out: the pass is over */
#define RADIO_SYSEX_ASK		0x14	/* in:  how is the notify path? */
#define RADIO_SYSEX_STATS	0x15	/* out: this is how */
#define RADIO_SYSEX_LISTENER	0x16	/* out: somebody is subscribed, or is not */
#define RADIO_SYSEX_PAIRING	0x17	/* in:  the pedal has opened its window */
#define RADIO_SYSEX_PAIRED	0x18	/* out: somebody bonded */
#define RADIO_SYSEX_FORGET	0x19	/* in:  drop every key */
#define RADIO_SYSEX_LAST	0x1f

#define RADIO_SYSEX_BODY	96

static void radio_sysex(const uint8_t *body, size_t len)
{
	uint8_t msg[3 + RADIO_SYSEX_BODY + 1];
	size_t n = 0;

	msg[n++] = 0xF0;
	msg[n++] = 0x7D;
	for (size_t i = 0; i < len && n < sizeof(msg) - 1; i++)
		msg[n++] = body[i] & 0x7f;
	msg[n++] = 0xF7;

	midi_uart_send(msg, n);
}

#ifdef CONFIG_BT_OBSERVER
void scan_found(const bt_addr_le_t *addr, const char *name)
{
	uint8_t body[1 + 1 + 12 + SCAN_NAME_MAX];
	size_t n = 0;

	body[n++] = RADIO_SYSEX_FOUND;

	//
	// Public or random, and it is not decoration: a connection is
	// made to the pair, and the same six bytes with the other type
	// reaches nothing.
	//
	body[n++] = addr->type & 0x7f;

	//
	// Most significant nibble first, and the bytes in the order
	// bt_addr_le_t holds them, so that what comes back out the other
	// end can be handed straight to bt_addr_le_t again.
	//
	for (int i = 0; i < 6; i++) {
		body[n++] = (addr->a.val[i] >> 4) & 0x0f;
		body[n++] = addr->a.val[i] & 0x0f;
	}

	for (const char *c = name; *c && n < sizeof(body); c++)
		if (*c >= 0x20 && *c < 0x7f)
			body[n++] = *c;

	radio_sysex(body, n);
}

void scan_done(unsigned int listed)
{
	uint8_t body[2] = {
		RADIO_SYSEX_DONE,
		listed & 0x7f,
	};

	radio_sysex(body, sizeof(body));
}
#endif

//
// One of ours, or MIDI passing through?
//
// Answered from the first two bytes after F0, so the rest of a message
// that is not ours streams to the packer as it arrives rather than
// having to be held whole.
//
/*
 * What the notify path did, when asked.
 *
 * The radio has no console and this is the only way it can say anything:
 * F0 7D 14 in is the request, F0 7D 15 out is the answer, and the pedal
 * forwards both without reading either.
 *
 * Asked rather than offered, because sending this on a timer changes
 * what it measures - a transfer that fails with the link otherwise idle
 * completes when there is 70 bytes of traffic every 500 ms.
 */
//
// How many keys the radio is holding.
//
// The first-order question about bonding, and the only way to tell a bond
// that survived a reset from a host quietly pairing again - which looks
// identical from the host.
//
static void count_bond(const struct bt_bond_info *info, void *user)
{
	ARG_UNUSED(info);
	(*(unsigned int *)user)++;
}

static unsigned int bonds(void)
{
	unsigned int n = 0;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &n);
	return n;
}

static void stats_send(void)
{
	uint8_t body[1 + RADIO_SYSEX_BODY];
	int n;

	/*
	 * Short keys because the pedal's inbound SysEx buffer is
	 * SYSEX_BUF_MAX - 192 bytes - and a message past it is dropped
	 * whole, silently from this end.
	 *
	 * f bytes in, o bytes notified, p packets, nc no connection,
	 * ns no slot, fl notify refused, e its error, r the pedal's
	 * backlog here, s whether the pedal is being held off, ccn how
	 * often a client asked to be sent anything, lo bytes the pedal
	 * sent that there was no room for, bo keys stored.
	 */
	body[0] = RADIO_SYSEX_STATS;	/* not ASK: the pedal echoes what we
					 * send back at us, and an answer that
					 * reads as a request answers itself
					 * for ever. */
	n = snprintf((char *)body + 1, RADIO_SYSEX_BODY,
		     "{\"f\":%u,\"o\":%u,\"p\":%u,\"nc\":%u,\"ns\":%u"
		     ",\"fl\":%u,\"e\":%d,\"r\":%u,\"s\":%u"
		     ",\"ccn\":%u,\"lo\":%u,\"bo\":%u}",
		     out.fed, out.notified, out.sent, out.noconn, out.noslot,
		     out.failed, out.err, midi_uart_backlog(),
		     midi_uart_halted(), out.ccc_n, midi_uart_lost(),
		     bonds());

	if (n > 0)
		radio_sysex(body, 1 + (size_t)n);
}

/*
 * Tell the pedal whether anything is listening, when that changes.
 *
 * Without it the pedal hands over a 36 kB schema that has nowhere to go
 * and is refused a packet at a time, which costs the link the whole
 * transfer and destroys it silently.  With it the pedal throws the reply
 * away at its own end instead - the same bargain midi_tx_push() already
 * makes with a USB host that is not mounted.
 *
 * From the main loop, because the transmit ring has one writer.  The
 * Bluetooth callback that notices the change only clears a flag.
 */
void midi_ble_notices(void)
{
	uint8_t body[2];
	bool on = (out.ccc == BT_GATT_CCC_NOTIFY) && midi_conn;

	//
	// A bond completed.  Sent from here rather than from the callback
	// that learnt it, because the transmit ring has one writer.
	//
	if (out.bonded_pending) {
		uint8_t done[1] = { RADIO_SYSEX_PAIRED };

		out.bonded_pending = false;
		radio_sysex(done, sizeof(done));
	}

	if (out.told && on == out.listening)
		return;

	body[0] = RADIO_SYSEX_LISTENER;
	body[1] = on;
	radio_sysex(body, sizeof(body));

	out.listening = on;
	out.told = true;
}

static bool radio_command(uint8_t cmd)
{
	return cmd >= RADIO_SYSEX_FIRST && cmd <= RADIO_SYSEX_LAST;
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

	/*
	 * The start of a SysEx, held back while it is decided whether it
	 * belongs to the radio.  'hold' is F0 and the two bytes after it.
	 */
	uint8_t hold[3];
	uint8_t held;
	bool mine;
	uint8_t cmd;
	uint8_t arg[16];
	uint8_t nr_arg;
} in;

//
// Not ours after all: put the start of the message back into the
// packet before the rest of it streams through.
//
static void release_held(void)
{
	if (!in.held)
		return;

	pack_sysex_start();
	for (uint8_t i = 1; i < in.held; i++)
		pack_sysex_data(in.hold[i]);
	in.held = 0;
}

static void radio_dispatch(uint8_t cmd, const uint8_t *arg, uint8_t len)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(len);

	switch (cmd) {
	case RADIO_SYSEX_ASK:
		stats_send();
		break;

	//
	// The pedal says whether it is offering to be paired with.
	//
	case RADIO_SYSEX_PAIRING:
		out.pairing = len >= 1 && arg[0];
		//
		// One rule and no second piece of state: a bond is
		// accepted while the pedal's window is open and at no
		// other time.  Which means binding a footswitch needs the
		// window open too - the app's own dialog for picking one
		// has to open it.
		//
		bt_set_bondable(out.pairing);
		break;

	//
	// Forget every key.
	//
	// Needed because a bond cannot be replaced by a peer that asks:
	// BT_SMP_ALLOW_UNAUTH_OVERWRITE would allow that, and it is exactly
	// what an attacker who copied the address would use, so the old key
	// has to be deleted deliberately instead.  Deliberately means over
	// USB, which is the only thing that can reach here.
	//
	// It is also the plain thing a player wants when a pedal has been
	// paired with something they no longer have.
	//
	case RADIO_SYSEX_FORGET:
		bt_unpair(BT_ID_DEFAULT, NULL);
		break;
#ifdef CONFIG_BT_OBSERVER
	case RADIO_SYSEX_SCAN:
		scan_start();
		break;
#endif
#ifdef CONFIG_BT_CENTRAL
	//
	// The address type, then the six bytes a nibble at a time, in
	// the order bt_addr_le_t holds them - the same shape a result
	// went out in.
	//
	case RADIO_SYSEX_BIND: {
		bt_addr_le_t addr;

		if (len < 13)
			break;

		addr.type = arg[0];
		for (int i = 0; i < 6; i++)
			addr.a.val[i] = (arg[1 + 2 * i] << 4) | arg[2 + 2 * i];

		bind_to(&addr);
		break;
	}
#endif
	default:
		break;
	}
}

void midi_ble_feed(uint8_t b)
{
	out.fed++;

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
		//
		// Held rather than packed, until the two bytes after F0
		// say whether this is MIDI passing through or a command
		// for the radio.  Two bytes is the whole lookahead.
		//
		in.sysex = true;
		in.held = 1;
		in.mine = false;
		in.want = 0;
		in.len = 0;
		in.status = 0;
		return;
	}

	if (b == 0xF7) {
		if (in.mine) {
			radio_dispatch(in.cmd, in.arg, in.nr_arg);
		} else if (in.sysex) {
			release_held();
			pack_sysex_end();
		}
		in.sysex = false;
		in.mine = false;
		in.held = 0;
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
	if (in.mine) {
		if (in.nr_arg < sizeof(in.arg))
			in.arg[in.nr_arg++] = b;
		return;
	}

	if (in.sysex) {
		if (in.held) {
			in.hold[in.held++] = b;

			//
			// F0 7D <cmd>: enough to know whose it is.
			//
			if (in.held == 3) {
				if (in.hold[1] == 0x7D && radio_command(b)) {
					in.mine = true;
					in.cmd = b;
					in.nr_arg = 0;
					in.held = 0;
					return;
				}
				release_held();
			}
			return;
		}
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

static void midi_ble_decode(const uint8_t *buf, uint16_t len);

/*
 * One packet from over the air, from the main loop.
 *
 * Not from the callback that received it: that runs in the Bluetooth
 * stack's own thread, at a cooperative priority it cannot be preempted
 * back into, so anything done there that waits stalls the receive path.
 * midi_ble_queue() copies the packet and this deals with it later.
 */
void midi_ble_packet(const uint8_t *buf, uint16_t len)
{
	midi_ble_decode(buf, len);
}

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
	midi_ble_queue(buf, len);
	return len;
}

static void midi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	out.ccc = value;
	out.ccc_n++;
	out.told = false;
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

//
// Both roles arrive here.  A connection this end opened belongs to the
// controller, and is nothing to do with the web app - treating one as
// the other would point the MIDI notifications at a footswitch and lose
// the app.
//
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (bind_owns(conn)) {
		bind_connected(conn, err);
		return;
	}

	if (err) {
		printk("bt: connect failed, %u\n", err);
		return;
	}

	midi_conn = bt_conn_ref(conn);
	printk("bt: connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (bind_owns(conn)) {
		bind_disconnected(conn, reason);
		return;
	}

	printk("bt: disconnected, %u\n", reason);
	out.ccc = 0;
	out.told = false;

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

#ifdef CONFIG_BT_CENTRAL
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (bind_owns(conn))
		bind_encrypted(conn, level, err);
}
#endif

//
// Somebody bonded.  Only the fact is wanted here - which peer it was is the
// radio's own business and the pedal has no use for it.
//
static void bonded(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	if (bonded)
		out.bonded_pending = true;
}

static struct bt_conn_auth_info_cb midi_auth_info = {
	.pairing_complete = bonded,
};

BT_CONN_CB_DEFINE(midi_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
#ifdef CONFIG_BT_CENTRAL
	.security_changed = security_changed,
#endif
};

void midi_ble_start(void)
{
	int err = bt_enable(NULL);

	if (err) {
		printk("bt: enable failed, %d\n", err);
		return;
	}

	//
	// Bring the stored keys back.  After bt_enable() and not before:
	// the host registers its own settings handlers during enable, and
	// a load that runs first finds nothing to give them to.
	//
	settings_load();
	bt_conn_auth_info_cb_register(&midi_auth_info);

	//
	// Closed until the pedal says otherwise.  Being in the room is not
	// an argument for being allowed to bond with somebody's pedal, and
	// the only thing that can open the window is a cable.
	//
	bt_set_bondable(false);

	adv_start(NULL);
}

#ifdef CONFIG_BT_CENTRAL
//
// What a bound controller sent.  The same decoder a write from the web
// app goes through, so both end at midi_uart_send() and the pedal never
// learns which it was.
//
void midi_ble_controller(const uint8_t *buf, uint16_t len)
{
	midi_ble_queue(buf, len);
}
#endif
