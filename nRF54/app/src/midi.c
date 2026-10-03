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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/sys/byteorder.h>

#include "midi.h"
#include "scan.h"
#include "bind.h"
#include "sink.h"

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

/*
 * The connection MIDI goes out on: the one that subscribed last.  Another
 * may be connected beside it and never ask for anything.
 *
 * Changed from the Bluetooth stack's thread and read from the loop, so it
 * is swapped and taken under a lock, and the loop holds a reference for as
 * long as it uses it.
 */
static struct bt_conn *midi_conn;
static struct k_spinlock midi_lock;

static struct bt_conn *midi_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&midi_lock);
	struct bt_conn *conn = midi_conn ? bt_conn_ref(midi_conn) : NULL;

	k_spin_unlock(&midi_lock, key);
	return conn;
}

/*
 * Each connection's number on the link to the pedal (link.h), handed out in
 * order as connections arrive - 1 to 255, skipping any still in use - so a
 * number is never reused while something addressed to its old owner could
 * still be on the way.  The pedal only echoes these back.
 */
static uint8_t peer_ids[CONFIG_BT_MAX_CONN];
static uint8_t peer_next = 1;

static void peer_assign(struct bt_conn *conn)
{
	uint8_t id;
	bool taken;

	do {
		id = peer_next++;
		if (!peer_next)
			peer_next = 1;
		taken = false;
		for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
			taken |= peer_ids[i] == id;
	} while (taken);
	peer_ids[bt_conn_index(conn)] = id;
}

struct peer_find {
	uint8_t id;
	struct bt_conn *found;
};

static void peer_find_one(struct bt_conn *conn, void *data)
{
	struct peer_find *f = data;

	if (!f->found && peer_ids[bt_conn_index(conn)] == f->id)
		f->found = bt_conn_ref(conn);
}

/* The connection a peer number belongs to, referenced, or NULL if gone */
static struct bt_conn *peer_conn(uint8_t id)
{
	struct peer_find f = { .id = id };

	bt_conn_foreach(BT_CONN_TYPE_LE, peer_find_one, &f);
	return f.found;
}

/*
 * Whether the pedal should trust what a connection sends: a host that
 * connected to us, over Secure Connections, with a bond stored for it -
 * which only the pairing window makes.  A device the radio connected to
 * itself is a controller, never an editor, whatever its keys.
 */
static bool peer_trusted(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (bind_owns(conn) || bt_conn_get_info(conn, &info))
		return false;
	return info.role == BT_CONN_ROLE_PERIPHERAL &&
	       (info.security.flags & BT_SECURITY_FLAG_SC) &&
	       bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn));
}

/*
 * Which peer the MIDI being packed is for: 0 for the connection that
 * subscribed last, as before peers had numbers.
 */
static uint8_t out_peer;

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
	 * How many times a client has written its subscription.  A client
	 * that never asked and a radio that will not send look the same
	 * from the client's side; this tells them apart.
	 */
	uint32_t ccc_n;

	/*
	 * Whether the pedal has been told what the line above says, and
	 * what it was told.  The change is noticed in a Bluetooth callback
	 * and sent from the main loop, because the transmit ring has one
	 * writer and that is the loop.
	 */
	bool told;
	bool listening;

	/*
	 * Hosts that connected, how many of those links ended with no
	 * packet ever heard (0x3e) or with the supervision timeout (0x08),
	 * and why the last one ended.
	 */
	uint32_t conns, dc3e, dc08;
	uint8_t last_reason;

	/*
	 * The last host that connected: who, the security level its link
	 * reached, the error if raising it failed, and why its last pairing
	 * failed - which between them say whether the pedal trusts it.
	 */
	bt_addr_le_t peer;
	uint8_t peer_level;
	bool peer_sc;
	int peer_err, pair_fail;

	/* Writes that reached the MIDI characteristic, from anyone */
	uint32_t writes;

	/* MIDI handed to the bound device, and how much of it was refused */
	uint32_t bound_sent, bound_failed;

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
 * actually gone.  Waiting for a slot is the back-pressure: the pedal's
 * MIDI stays in main.c unacknowledged, and the link's window stops the
 * pedal sending more.
 */
#define MIDI_BLE_SLOTS	CONFIG_BT_ATT_TX_COUNT

static struct {
	uint8_t buf[MIDI_BLE_MAX_PKT];
	struct bt_gatt_notify_params params;
	struct bt_conn *conn;		/* compared, never dereferenced */
} slot[MIDI_BLE_SLOTS];

static ATOMIC_DEFINE(slot_busy, MIDI_BLE_SLOTS);
static K_SEM_DEFINE(slot_free, MIDI_BLE_SLOTS, MIDI_BLE_SLOTS);

//
// A slot is given back once whichever comes first: its completion, or its
// connection going.  The ATT layer drops the completion of a notification
// whose bearer has gone, so a disconnect has to give back what that
// connection held - and only that, since another connection's packets may
// still be in flight.
//
static void slot_give(unsigned int i)
{
	if (atomic_test_and_clear_bit(slot_busy, i))
		k_sem_give(&slot_free);
}

static void notify_done(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);

	slot_give(POINTER_TO_UINT(user_data));
}

static void slot_release(struct bt_conn *conn)
{
	for (unsigned int i = 0; i < MIDI_BLE_SLOTS; i++)
		if (slot[i].conn == conn)
			slot_give(i);
}

static int slot_take(void)
{
	for (unsigned int i = 0; i < MIDI_BLE_SLOTS; i++)
		if (!atomic_test_and_set_bit(slot_busy, i))
			return i;
	return -1;
}

static uint16_t midi_ble_now(void)
{
	return (uint16_t)(k_uptime_get_32() & 0x1FFF);
}

static uint16_t midi_ble_limit(void)
{
	struct bt_conn *conn = midi_get();
	uint16_t mtu;

	if (!conn)
		return MIDI_BLE_MIN_PKT;

	mtu = bt_gatt_get_mtu(conn);
	bt_conn_unref(conn);
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
	struct bt_conn *conn;
	int err, i;

	/*
	 * A header byte on its own carries nothing.  This is the ordinary
	 * case - the sender calls here whenever the UART goes quiet.
	 */
	if (out.len <= 1) {
		out.len = 0;
		return;
	}

	conn = out_peer ? peer_conn(out_peer) : midi_get();
	if (!conn) {
		out.noconn++;
		out.len = 0;
		return;
	}

	/*
	 * Never waits.  A caller that asked midi_ble_ready() first has a
	 * slot, so this cannot fire; 'noslot' reading non-zero means
	 * somebody added a caller that does not ask.
	 *
	 * Waiting here would pause the loop, and every stream with it.
	 * Leaving the MIDI unacknowledged is what holds the pedal off.
	 */
	if (k_sem_take(&slot_free, K_NO_WAIT) != 0) {
		out.noslot++;
		out.dropped++;
		out.len = 0;
		bt_conn_unref(conn);
		return;
	}

	/* The count says one is free, so this finds it */
	i = slot_take();
	if (i < 0) {
		k_sem_give(&slot_free);
		out.noslot++;
		out.dropped++;
		out.len = 0;
		bt_conn_unref(conn);
		return;
	}

	memcpy(slot[i].buf, out.buf, out.len);
	slot[i].conn = conn;
	slot[i].params = (struct bt_gatt_notify_params){
		.attr = midi_value_attr(),
		.data = slot[i].buf,
		.len  = out.len,
		.func = notify_done,
		.user_data = UINT_TO_POINTER(i),
	};

	err = bt_gatt_notify_cb(conn, &slot[i].params);
	if (err) {
		slot_give(i);
		out.failed++;
		out.err = err;
		out.dropped++;
	} else {
		out.sent++;
		out.notified += out.len;
	}

	out.len = 0;
	bt_conn_unref(conn);
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
 * through it.  Both directions go on the link's control stream, so they
 * never meet the MIDI on its way to Bluetooth.
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
#define RADIO_SYSEX_FORGET	0x19	/* in:  drop one key, or all of them */
#define RADIO_SYSEX_BONDS	0x1a	/* in:  what are you paired with? */
#define RADIO_SYSEX_BOND	0x1b	/* out: one of them */
#define RADIO_SYSEX_BONDS_END	0x1c	/* out: that is all of them */
#define RADIO_SYSEX_SEND	0x1d	/* in:  MIDI for the bound device */
#define RADIO_SYSEX_LINKS	0x1e	/* out: connections, and how they ended */
#define RADIO_SYSEX_NAME	0x1f	/* in:  what to advertise as */
#define RADIO_SYSEX_LAST	0x1f

static void name_set(const uint8_t *arg, uint8_t len);

#define RADIO_SYSEX_BODY	184

static void radio_sysex(const uint8_t *body, size_t len)
{
	uint8_t msg[3 + RADIO_SYSEX_BODY + 1];
	size_t n = 0;

	msg[n++] = 0xF0;
	msg[n++] = 0x7D;
	for (size_t i = 0; i < len && n < sizeof(msg) - 1; i++)
		msg[n++] = body[i] & 0x7f;
	msg[n++] = 0xF7;

	midi_uart_control(msg, n);
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

//
// One key, on its way to whoever asked.
//
// The same encoding a scan result uses - address type, then the six bytes
// a nibble at a time - so the app decodes both with one function and can
// hand either straight back as something to forget.  No name: a key is
// stored against an address and the radio never knew what it was called.
//
static void list_bond(const struct bt_bond_info *info, void *user)
{
	uint8_t body[1 + 1 + 12];
	size_t n = 0;

	ARG_UNUSED(user);

	body[n++] = RADIO_SYSEX_BOND;
	body[n++] = info->addr.type & 0x7f;
	for (int i = 0; i < 6; i++) {
		body[n++] = (info->addr.a.val[i] >> 4) & 0x0f;
		body[n++] = info->addr.a.val[i] & 0x0f;
	}
	radio_sysex(body, n);
}

//
// An address out of the twelve nibbles one arrived as.
//
static bool address_from(const uint8_t *arg, uint8_t len, bt_addr_le_t *addr)
{
	if (len < 13)
		return false;

	addr->type = arg[0];
	for (int i = 0; i < 6; i++)
		addr->a.val[i] = (arg[1 + 2 * i] << 4) | arg[2 + 2 * i];
	return true;
}

/*
 * The signal strength of the connection MIDI goes to, in dBm, as this
 * end's controller measures it.  0 when there is none.
 */
static int midi_rssi(void)
{
	struct bt_conn *conn = midi_get();
	struct bt_hci_cp_read_rssi *cp;
	struct net_buf *buf, *rsp = NULL;
	uint16_t handle;
	int rssi = 0;

	if (!conn)
		return 0;

	buf = bt_hci_get_conn_handle(conn, &handle) ? NULL :
	      bt_hci_cmd_alloc(K_MSEC(50));
	if (buf) {
		cp = net_buf_add(buf, sizeof(*cp));
		cp->handle = sys_cpu_to_le16(handle);
		if (!bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp)) {
			rssi = ((struct bt_hci_rp_read_rssi *)rsp->data)->rssi;
			net_buf_unref(rsp);
		}
	}

	bt_conn_unref(conn);
	return rssi;
}

/*
 * One reply of JSON to the pedal.  Short keys, because the pedal's
 * inbound SysEx buffer is SYSEX_BUF_MAX - 192 bytes - and a message past
 * it is dropped whole, silently from this end.  A reply the buffer here
 * would cut short is replaced by a marker, since cut short it would not
 * parse.
 */
static void radio_json(uint8_t cmd, const char *fmt, ...)
{
	uint8_t body[1 + RADIO_SYSEX_BODY];
	va_list ap;
	int n;

	body[0] = cmd;
	va_start(ap, fmt);
	n = vsnprintf((char *)body + 1, RADIO_SYSEX_BODY, fmt, ap);
	va_end(ap);

	if (n >= RADIO_SYSEX_BODY)
		n = snprintf((char *)body + 1, RADIO_SYSEX_BODY,
			     "{\"truncated\":true}");
	if (n > 0)
		radio_sysex(body, 1 + (size_t)n);
}

/*
 * The counters, in two replies so that each stays well inside the
 * buffer.  Not answered as ASK: the pedal echoes what we send back at
 * us, and an answer that reads as a request answers itself for ever.
 *
 * f bytes in, o bytes notified, p packets, nc no connection, ns no slot,
 * fl notify refused, e its error, r the pedal's backlog here, ccn how
 * often a client asked to be sent anything, lo bytes from the pedal
 * overwritten before they were read, bo keys stored, rs the signal
 * strength of the MIDI connection in dBm, and for the link to the pedal
 * lg packets that never arrived, lr packets with nowhere to go, lb packets
 * malformed, lt bytes for the pedal there was no room for, lw packets it
 * sent and then took as lost, lc packets dropped for a wrong CRC, and rb
 * bytes the UART has received, all told.
 *
 * Then cn connections accepted, 3e and 08 how many of them ended for
 * those reasons, dr why the last one ended, bs and bf MIDI sent to the
 * bound device and refused, bd how far the bind has got, and be why it
 * last failed.
 */
static void stats_send(void)
{
	uint32_t gaps, refused, bad, lost;

	midi_uart_link_counts(&gaps, &refused, &bad, &lost);
	radio_json(RADIO_SYSEX_STATS,
		   "{\"f\":%u,\"o\":%u,\"p\":%u,\"nc\":%u,\"ns\":%u"
		   ",\"fl\":%u,\"e\":%d,\"r\":%u"
		   ",\"ccn\":%u,\"lo\":%u,\"bo\":%u,\"rs\":%d"
		   ",\"lg\":%u,\"lr\":%u,\"lb\":%u,\"lt\":%u"
		   ",\"lw\":%u,\"lc\":%u,\"rb\":%u}",
		   out.fed, out.notified, out.sent, out.noconn, out.noslot,
		   out.failed, out.err, midi_uart_backlog(),
		   out.ccc_n, midi_uart_lost(),
		   bonds(), midi_rssi(), gaps, refused, bad, lost,
		   midi_uart_written_off(), midi_uart_crc_failed(),
		   midi_uart_received());

	char peer[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&out.peer, peer, sizeof(peer));
	radio_json(RADIO_SYSEX_LINKS,
		   "{\"cn\":%u,\"3e\":%u,\"08\":%u,\"dr\":%u"
		   ",\"bs\":%u,\"bf\":%u,\"bd\":%u,\"be\":%d"
		   ",\"pa\":\"%s\",\"pl\":%u,\"sc\":%u,\"pe\":%d"
		   ",\"pf\":%d,\"w\":%u}",
		   out.conns, out.dc3e, out.dc08, out.last_reason,
		   out.bound_sent, out.bound_failed, bind_state(),
		   bind_last_err(), peer, out.peer_level, out.peer_sc,
		   out.peer_err, out.pair_fail, out.writes);
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
	bool on = midi_conn != NULL;

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
} in;

static void radio_dispatch(uint8_t cmd, const uint8_t *arg, uint8_t len)
{
	ARG_UNUSED(arg);
	ARG_UNUSED(len);

	switch (cmd) {
	case RADIO_SYSEX_ASK:
		stats_send();
		break;

	case RADIO_SYSEX_NAME:
		name_set(arg, len);
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
	//
	// Forget one key, or every one of them when no address is given.
	// The address arrives as a scan result's does, so what was listed
	// can be handed straight back.
	//
	case RADIO_SYSEX_FORGET: {
		bt_addr_le_t addr;

		if (address_from(arg, len, &addr))
			bt_unpair(BT_ID_DEFAULT, &addr);
		else
			bt_unpair(BT_ID_DEFAULT, NULL);
		break;
	}

	//
	// What the radio is paired with: one message each and then an end,
	// so an empty answer is still an answer.
	//
	case RADIO_SYSEX_BONDS: {
		uint8_t end[2] = { RADIO_SYSEX_BONDS_END, 0 };

		bt_foreach_bond(BT_ID_DEFAULT, list_bond, NULL);
		end[1] = bonds() & 0x7f;
		radio_sysex(end, sizeof(end));
		break;
	}
#ifdef CONFIG_BT_OBSERVER
	//
	// A bare 0x10, or 0x10 01, looks; 0x10 00 stops looking.  Stopping
	// reports what was seen rather than throwing it away, so pressing it
	// the moment a controller appears is the ordinary way to use it.
	//
	case RADIO_SYSEX_SCAN:
		if (len >= 1 && !arg[0])
			scan_stop();
		else
			scan_start();
		break;
#endif
#ifdef CONFIG_BT_CENTRAL
	//
	// MIDI for the device bound to, a byte as two nibbles because a
	// status byte cannot travel inside SysEx.  More than fits is
	// refused whole rather than sent in part.
	//
	case RADIO_SYSEX_SEND: {
		uint8_t midi[BIND_SEND_MAX];
		size_t n = len / 2;

		for (size_t i = 0; i < n && i < BIND_SEND_MAX; i++)
			midi[i] = (arg[2 * i] << 4) | arg[2 * i + 1];
		if (n && n <= BIND_SEND_MAX && !bind_send(midi, n))
			out.bound_sent++;
		else
			out.bound_failed++;
		break;
	}

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

/*
 * The MIDI fed from here on is for this peer.  A packet half built for
 * another goes first, so the two never share one; the caller has asked
 * midi_ble_ready(), so there is a slot for it.
 */
void midi_ble_to(uint8_t peer)
{
	if (peer == out_peer)
		return;
	midi_ble_resync();
	midi_ble_flush();
	out_peer = peer;
}

/*
 * What comes next is not the rest of the message being parsed: a gap on
 * the link cut it short, or the pedal has gone on to another peer, which it
 * only does between messages.  A SysEx is closed rather than left for the
 * far end to wait on, and anything half collected is forgotten.
 */
void midi_ble_resync(void)
{
	if (in.sysex)
		pack_sysex_end();
	in.sysex = false;
	in.len = 0;
	in.want = 0;
	in.status = 0;
}

/*
 * A command from the pedal, whole: F0 7D, the command, its arguments and
 * F7, off the control stream.  It never meets the MIDI parser, so it cannot
 * land in the middle of a SysEx on its way to the air.
 */
void midi_radio_command(const uint8_t *msg, size_t len)
{
	if (len < 4 || msg[0] != 0xF0 || msg[1] != 0x7D ||
	    msg[len - 1] != 0xF7 || !radio_command(msg[2]))
		return;
	radio_dispatch(msg[2], msg + 3, (uint8_t)(len - 4));
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
		if (in.sysex)
			pack_sysex_end();
		pack_sysex_start();
		in.sysex = true;
		in.want = 0;
		in.len = 0;
		in.status = 0;
		return;
	}

	if (b == 0xF7) {
		if (in.sysex)
			pack_sysex_end();
		in.sysex = false;
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

/* Running status, per connection: two senders must not share one */
static uint8_t dec_statuses[CONFIG_BT_MAX_CONN];

static void midi_ble_decode(uint8_t *dec_status, const uint8_t *buf,
			    uint16_t len);

/*
 * One packet from over the air, from the main loop, with the connection it
 * came on.
 *
 * Not from the callback that received it: that runs in the Bluetooth
 * stack's own thread, at a cooperative priority it cannot be preempted
 * back into, so anything done there that waits stalls the receive path.
 * midi_ble_queue() copies the packet and this deals with it later.
 */
void midi_ble_packet(uint8_t src, const uint8_t *buf, uint16_t len)
{
	if (src < CONFIG_BT_MAX_CONN)
		midi_ble_decode(&dec_statuses[src], buf, len);
}

/*
 * A packet arriving, from the Bluetooth stack's thread: queued for the loop
 * with which connection it came on, its peer number and whether to trust
 * it, all of which are known only now.
 */
static void midi_ble_arrived(struct bt_conn *conn, const uint8_t *buf,
			     uint16_t len)
{
	midi_ble_queue(bt_conn_index(conn), peer_ids[bt_conn_index(conn)],
		       peer_trusted(conn) ? MIDI_FROM_TRUSTED : 0, buf, len);
}

static void midi_ble_decode(uint8_t *dec_status, const uint8_t *buf,
			    uint16_t len)
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
				*dec_status = status;
			else if (status >= 0xF8)
				;	/* real-time cancels nothing */
			else
				*dec_status = 0;
		} else if (*dec_status) {
			/* Running status: the status byte was omitted. */
			midi_uart_send(dec_status, 1);
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
	out.writes++;
	midi_ble_arrived(conn, buf, len);
	return len;
}

static void midi_set(struct bt_conn *conn)
{
	struct bt_conn *old;
	k_spinlock_key_t key;

	if (conn == midi_conn)
		return;

	key = k_spin_lock(&midi_lock);
	old = midi_conn;
	midi_conn = conn ? bt_conn_ref(conn) : NULL;
	k_spin_unlock(&midi_lock, key);

	if (old)
		bt_conn_unref(old);
	out.told = false;
}

struct midi_pick {
	struct bt_conn *except, *found;
};

static void midi_pick_one(struct bt_conn *conn, void *data)
{
	struct midi_pick *pick = data;
	struct bt_conn_info info;

	if (pick->found || conn == pick->except || bind_owns(conn))
		return;
	if (bt_conn_get_info(conn, &info) ||
	    info.state != BT_CONN_STATE_CONNECTED)
		return;
	if (bt_gatt_is_subscribed(conn, midi_value_attr(), BT_GATT_CCC_NOTIFY))
		pick->found = conn;
}

// Another subscribed connection, for when this one stops being the one
static struct bt_conn *midi_pick(struct bt_conn *except)
{
	struct midi_pick pick = { .except = except };

	bt_conn_foreach(BT_CONN_TYPE_LE, midi_pick_one, &pick);
	return pick.found;
}

//
// A client subscribing or unsubscribing, which is the only place that
// says which connection it was.
//
static ssize_t midi_ccc_write(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	out.ccc_n++;
	if (value & BT_GATT_CCC_NOTIFY)
		midi_set(conn);
	else if (conn == midi_conn)
		midi_set(midi_pick(conn));
	return sizeof(value);
}

//
// Whether anybody at all is subscribed.  A bonded client's subscription
// is restored when it reconnects without anything being written, and this
// is the only sign of it.  It never clears the connection: a disconnect
// can arrive here before disconnected() does, which has to know which
// connection was the one MIDI went to.
//
static void midi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	if (value && !midi_conn)
		midi_set(midi_pick(NULL));
}

//
// Anyone may write, and only a bond made with LE Secure Connections may
// subscribe.
//
// Writing is open because a footswitch may pair the legacy way or not at
// all, and it only ever says "somebody pressed me".  What it may say is the
// pedal's decision: each packet goes to the pedal marked trusted or not
// (peer_trusted()), and the pedal takes only channel messages from a peer
// that is not.  Subscribing is what gets a peer the pedal's answers, so
// that stays behind the bond.
//
// BT_GATT_PERM_*_LESC asks for a stored key with the Secure Connections
// flag, so an encrypted session with no bond behind it does not qualify -
// which is exactly the case a closed pairing window produces.
//
// The configuration descriptor carries it too, so a host that cannot be
// sent anything is told when it subscribes rather than subscribing
// happily and then waiting for a notification that will never come.  That
// silence was the worst failure this link had: a connection that looked
// established and did nothing.
//
BT_GATT_SERVICE_DEFINE(midi_svc,
	BT_GATT_PRIMARY_SERVICE(&midi_service_uuid),
	BT_GATT_CHARACTERISTIC(&midi_io_uuid.uuid,
			       BT_GATT_CHRC_READ |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP |
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_LESC |
			       BT_GATT_PERM_WRITE,
			       midi_read, midi_write, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(midi_ccc_changed, midi_ccc_write,
				  BT_GATT_PERM_READ_LESC |
				  BT_GATT_PERM_WRITE_LESC),
);

static const struct bt_gatt_attr *midi_value_attr(void)
{
	return &midi_svc.attrs[1];
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

/*
 * Flags, the start of the name, and the service UUID: 3 + 7 + 18 bytes
 * of the 31 an advertisement has.  The UUID is in there because that is
 * what makes a scanner call this a MIDI device rather than an unknown
 * one.
 *
 * The whole name is in the scan response, as much of it as fits beside
 * what else is there: 14 characters.  A scanner that asks for it shows
 * that, and one that does not shows "Pedal".
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_SHORTENED, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MIDI_SERVICE_VAL),
};

static char adv_name[CONFIG_BT_DEVICE_NAME_MAX + 1] = CONFIG_BT_DEVICE_NAME;

/*
 * After the name, the two LE Audio announcements, 5 and 10 bytes: the
 * Common Audio Service, and the stream endpoints with what each way is
 * available for, as sink.h has it.
 */
static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
#ifdef CONFIG_BT_BAP_UNICAST_SERVER
	BT_DATA_BYTES(BT_DATA_SVC_DATA16,
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_TARGETED),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16,
		      BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_TARGETED,
		      BT_BYTES_LIST_LE16(SINK_AVAILABLE),
		      BT_BYTES_LIST_LE16(SOURCE_AVAILABLE),
		      0x00),
#endif
};

/* The name in the scan response, cut short to leave room for the rest */
static void sd_name(void)
{
	size_t room = 31 - 2;

	for (size_t i = 1; i < ARRAY_SIZE(sd); i++)
		room -= 2 + sd[i].data_len;
	sd[0].data_len = MIN(strlen(adv_name), room);
	sd[0].type = strlen(adv_name) > room ? BT_DATA_NAME_SHORTENED :
					       BT_DATA_NAME_COMPLETE;
}

/*
 * Hosts connected to us, as against controllers we connected to.
 */
static atomic_t peers;

/*
 * Advertise whenever another host could connect.
 *
 * Advertising stops when a host connects, so it is started again after
 * every connection that leaves a slot free as well as after every
 * disconnect.  It is never started from the connection callbacks
 * themselves: they run in the stack's own thread, which may still be
 * unwinding the connection, and Zephyr names the 'recycled' callback as
 * the point where a connectable advertiser can start.
 *
 * And it is retried until it starts: a radio that is not advertising
 * cannot be found.
 */
static void adv_start(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_work, adv_start);

/*
 * Slowly while a phone streams audio: every advertising event takes the
 * radio from the stream, and at the fast rate a fifth of its packets were
 * lost.  Once a second still finds the pedal, a little later.
 */
static bool quiet, quiet_changed;

static void adv_start(struct k_work *work)
{
	int err;

	if (quiet_changed) {
		quiet_changed = false;
		bt_le_adv_stop();
	}
	if (atomic_get(&peers) >= CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT)
		return;

	err = bt_le_adv_start(quiet ?
			      BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
					      BT_GAP_ADV_SLOW_INT_MIN,
					      BT_GAP_ADV_SLOW_INT_MAX, NULL) :
			      BT_LE_ADV_CONN_FAST_1,
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err && err != -EALREADY) {
		printk("bt: advertising failed, %d\n", err);
		k_work_reschedule(&adv_work, K_MSEC(250));
	}
}

static void adv_again(void)
{
	k_work_reschedule(&adv_work, K_NO_WAIT);
}

/* From the Bluetooth stack's thread; adv_start() picks it up */
void midi_ble_quiet(bool q)
{
	if (q == quiet)
		return;
	quiet = q;
	quiet_changed = true;
	adv_again();
}

/*
 * The name the pedal asked for, applied on the system work queue.
 * adv_start() runs there and hands 'sd' to the stack, so changing the
 * name there too means it is never changed while being read.
 *
 * Both the GAP name, which a host reads after connecting, and the scan
 * response.  Updating the advertisement fails with -EAGAIN when it is not
 * running, and that is fine: the next adv_start() sends the new one.
 */
static char name_next[CONFIG_BT_DEVICE_NAME_MAX + 1];
static K_MUTEX_DEFINE(name_lock);

static void name_apply(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&name_lock, K_FOREVER);
	strcpy(adv_name, name_next);
	k_mutex_unlock(&name_lock);

	sd_name();
	bt_set_name(adv_name);
	bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

static K_WORK_DEFINE(name_work, name_apply);

/*
 * Printable ASCII only; a name with any other byte in it is refused.
 */
static void name_set(const uint8_t *arg, uint8_t len)
{
	if (!len || len > CONFIG_BT_DEVICE_NAME_MAX)
		return;
	for (uint8_t i = 0; i < len; i++)
		if (arg[i] < 0x20 || arg[i] > 0x7e)
			return;

	k_mutex_lock(&name_lock, K_FOREVER);
	memcpy(name_next, arg, len);
	name_next[len] = '\0';
	k_mutex_unlock(&name_lock);

	k_work_submit(&name_work);
}

//
// Both roles arrive here.  A connection this end opened belongs to the
// controller, and is nothing to do with the web app - treating one as
// the other would point the MIDI notifications at a footswitch and lose
// the app.
//
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (!err) {
		peer_assign(conn);
		dec_statuses[bt_conn_index(conn)] = 0;
	}

	if (bind_owns(conn)) {
		bind_connected(conn, err);
		return;
	}

	if (err) {
		printk("bt: connect failed, %u\n", err);
		adv_again();
		return;
	}

	atomic_inc(&peers);
	out.conns++;
	bt_addr_le_copy(&out.peer, bt_conn_get_dst(conn));
	out.peer_level = BT_SECURITY_L1;
	out.peer_sc = false;
	out.peer_err = out.pair_fail = 0;
	printk("bt: connected\n");
	adv_again();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	peer_ids[bt_conn_index(conn)] = 0;

	if (bind_owns(conn)) {
		bind_disconnected(conn, reason);
		return;
	}

	printk("bt: disconnected, %u\n", reason);
	atomic_dec(&peers);
	out.last_reason = reason;
	if (reason == BT_HCI_ERR_CONN_FAIL_TO_ESTAB)
		out.dc3e++;
	else if (reason == BT_HCI_ERR_CONN_TIMEOUT)
		out.dc08++;

	/*
	 * Outstanding notifications go with the connection, and their
	 * callbacks may never run, so its slots are given back by hand.
	 */
	slot_release(conn);

	if (conn == midi_conn) {
		midi_set(midi_pick(conn));

		/* A part-built packet has nowhere to go now. */
		out.len = 0;
		in.sysex = false;
		in.len = in.want = 0;
		in.status = 0;
	}

	adv_again();
}

static void recycled(void)
{
	adv_again();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
#ifdef CONFIG_BT_CENTRAL
	if (bind_owns(conn)) {
		bind_encrypted(conn, level, err);
		return;
	}
#endif
	if (bt_addr_le_eq(bt_conn_get_dst(conn), &out.peer)) {
		struct bt_conn_info info;

		out.peer_level = level;
		out.peer_err = err;
		out.peer_sc = !bt_conn_get_info(conn, &info) &&
			      (info.security.flags & BT_SECURITY_FLAG_SC);
	}
}

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

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (bt_addr_le_eq(bt_conn_get_dst(conn), &out.peer))
		out.pair_fail = reason;
}

static struct bt_conn_auth_info_cb midi_auth_info = {
	.pairing_complete = bonded,
	.pairing_failed = pairing_failed,
};

BT_CONN_CB_DEFINE(midi_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
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
	// The name the pedal gave last time, which the load just brought
	// back, so that a radio reset on its own keeps it.  Nothing is
	// advertising yet to read 'sd'.
	//
	strncpy(adv_name, bt_get_name(), CONFIG_BT_DEVICE_NAME_MAX);
	sd_name();

	//
	// Closed until the pedal says otherwise.  Being in the room is not
	// an argument for being allowed to bond with somebody's pedal, and
	// the only thing that can open the window is a cable.
	//
	bt_set_bondable(false);

	adv_again();
}

#ifdef CONFIG_BT_CENTRAL
//
// What a bound controller sent.  The same decoder a write from the web
// app goes through, so both end at midi_uart_send() and the pedal never
// learns which it was.
//
void midi_ble_controller(struct bt_conn *conn, const uint8_t *buf,
			 uint16_t len)
{
	midi_ble_arrived(conn, buf, len);
}
#endif
