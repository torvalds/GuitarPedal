/*
 * The UART side of the bridge: uart30 carries a plain MIDI byte stream
 * to and from the RP2354.  midi.c is the Bluetooth side.
 *
 * It carries nothing else.  There is no console anywhere on this radio -
 * prj.conf says why.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/random/random.h>

#include "midi.h"
#include "link.h"
#include "linktest.h"

/*
 * Reading RTT stalls the core long enough to miss Bluetooth connection
 * events, so a log kept over the debug pins breaks the link it is there
 * to explain - 6 to 20 disconnects per run against 0 or 1 without it.
 * prj.conf turns the console off entirely; this is what stops anything
 * turning that one back on quietly.
 */
#ifdef CONFIG_USE_SEGGER_RTT
#error "RTT costs Bluetooth connections - see prj.conf"
#endif

static const struct device *const link =
	DEVICE_DT_GET(DT_CHOSEN(pedal_midi_uart));

/* link.h and the devicetree have to agree about RTS/CTS */
#if LINK_FLOW_CONTROL != DT_PROP(DT_CHOSEN(pedal_midi_uart), hw_flow_control)
#error "LINK_FLOW_CONTROL in link.h disagrees with the devicetree's hw-flow-control"
#endif

/*
 * Buffers for the asynchronous UART API - see prj.conf for why it is
 * that one.
 *
 * Four, so that one is free whenever the driver asks: it asks for the next
 * buffer as it starts on one, which can be before it has given back the one
 * it finished, and with none to give it stops receiving.
 *
 * The timeout is what stops a short message waiting for a full buffer:
 * without it a three-byte controller change sits here until 256 bytes
 * have arrived, which on a quiet link is never.
 */
#define RX_BUFS		4
#define RX_BUF_LEN	256
#define RX_TIMEOUT_US	200	/* 20 byte-times at 1 Mbit */

static uint8_t rx_buf[RX_BUFS][RX_BUF_LEN];

/*
 * Which buffers the driver does not currently hold.
 *
 * A flag each rather than an index of the last one released: the driver
 * asks for the next buffer when it starts using one, not when it gives
 * one back, so the two events do not alternate and an index is wrong as
 * soon as two requests arrive together.  Handing out a buffer that is
 * already being filled loses whatever was in it.
 */
static bool rx_free[RX_BUFS];

static uint8_t *rx_take(void)
{
	for (int i = 0; i < RX_BUFS; i++) {
		if (rx_free[i]) {
			rx_free[i] = false;
			return rx_buf[i];
		}
	}
	return NULL;
}

/*
 * Packets that arrived over Bluetooth, waiting for the loop.
 *
 * A GATT callback runs in the Bluetooth stack's own thread, so it copies
 * the packet in here and returns rather than decoding it and writing to
 * the UART.  The RP2354 side follows the same rule, for the same reason:
 * see "Handle incoming MIDI from the main loop, not from a callback".
 *
 * One byte of length then the packet, so a reader knows where each ends.
 * A packet is at most the ATT MTU less three, which is under 256.
 *
 * It does not need to be big.  The controller can only hold
 * BT_MAX_CONN + BT_BUF_ACL_RX_COUNT_EXTRA buffers before the host stops
 * it, the largest message the pedal is ever sent is a full rule table at
 * 96 bytes, and there is no bulk inbound direction at all.
 */
RING_BUF_DECLARE(ble_in_ring, 1024);
static uint32_t ble_in_dropped;

/*
 * A ring each way: the driver fills one and empties the other, and the
 * loop below does the opposite.
 *
 * The receive ring is the one that has to be sized.  A SysEx schema is
 * tens of kilobytes arriving at 1 Mbit while Bluetooth carries it away
 * more slowly, and the difference collects here.  rx_starved() is what
 * happens when it fills.
 */
RING_BUF_DECLARE(uart_rx_ring, 4096);
RING_BUF_DECLARE(uart_tx_ring, 1024);

static bool tx_busy;
static uint32_t tx_claimed;
static bool rx_stopped;
static uint32_t rx_stops;	/* times RTS went up because buffers ran out */
static uint32_t rx_bytes;	/* bytes the driver has delivered */
static uint32_t rx_pos;		/* bytes the link has taken off the ring */

/*
 * Bytes the pedal sent that there was no room for.
 *
 * ring_buf_put() takes what fits and says how much, and ignoring that is
 * how a byte disappears with nobody the wiser.
 */
static uint32_t rx_lost;

/*
 * Is there room for another buffer's worth?
 *
 * Answering "no" is how RTS gets deasserted: the driver is refused a
 * buffer, the current one runs out, and a UARTE with nowhere to put the
 * next byte stops the far end instead of losing it.  Discarding here
 * would drop most of a schema and report nothing.
 *
 * Room for every buffer the driver may still be holding, not just for
 * one: it has one active and one already queued when it requests the
 * next, so up to RX_BUFS bufferfuls can still arrive after the
 * refusal.
 */
static bool rx_starved(void)
{
	return ring_buf_space_get(&uart_rx_ring) < RX_BUFS * RX_BUF_LEN;
}

static void tx_kick(void)
{
	uint8_t *data;
	uint32_t len;

	if (tx_busy)
		return;

	len = ring_buf_get_claim(&uart_tx_ring, &data, UINT32_MAX);
	if (!len)
		return;

	tx_busy = true;
	tx_claimed = len;
	uart_tx(link, data, len, SYS_FOREVER_US);
}

static void uart_cb(const struct device *dev, struct uart_event *evt,
		    void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_RX_RDY: {
		uint32_t took;

		rx_bytes += evt->data.rx.len;
		took = ring_buf_put(&uart_rx_ring,
					     evt->data.rx.buf +
					     evt->data.rx.offset,
					     evt->data.rx.len);

		if (took < evt->data.rx.len)
			rx_lost += evt->data.rx.len - took;
		break;
	}

	case UART_RX_BUF_REQUEST: {
		uint8_t *buf;

		if (rx_starved())
			break;
		buf = rx_take();
		if (buf)
			uart_rx_buf_rsp(dev, buf, RX_BUF_LEN);
		break;
	}

	case UART_RX_BUF_RELEASED:
		for (int i = 0; i < RX_BUFS; i++)
			if (evt->data.rx_buf.buf == rx_buf[i])
				rx_free[i] = true;
		break;

	case UART_RX_STOPPED:
		printk("uart: receive stopped, reason %d\n",
		       evt->data.rx_stop.reason);
		break;

	case UART_RX_DISABLED:
		rx_stopped = true;
		rx_stops++;
		break;

	case UART_TX_DONE:
	case UART_TX_ABORTED:
		ring_buf_get_finish(&uart_tx_ring, tx_claimed);
		tx_claimed = 0;
		tx_busy = false;
		break;

	default:
		break;
	}
}

/*
 * One packet from over the air, held for the loop to deal with.
 *
 * Called from the Bluetooth stack's thread, and does nothing but copy.
 * Whole packet or none: a partial one would be read back as a length and
 * then somebody else's bytes.
 */
void midi_ble_queue(uint8_t src, uint8_t peer, uint8_t flags,
		    const uint8_t *buf, uint16_t len)
{
	uint8_t hdr[4] = { (uint8_t)len, src, peer, flags };

	if (!len || len > 255 ||
	    ring_buf_space_get(&ble_in_ring) < sizeof(hdr) + len) {
		ble_in_dropped++;
		return;
	}
	ring_buf_put(&ble_in_ring, hdr, sizeof(hdr));
	ring_buf_put(&ble_in_ring, buf, len);
}

static void air_begin(uint8_t peer, uint8_t flags);
static void air_end(void);

/*
 * Hand the next waiting packet to the decoder, and say whether there was
 * one.  Called from the loop, so this is the only place that decodes.
 */
static bool ble_in_drain(void)
{
	uint8_t pkt[256];
	uint8_t hdr[4];

	if (ring_buf_get(&ble_in_ring, hdr, sizeof(hdr)) != sizeof(hdr))
		return false;
	if (ring_buf_get(&ble_in_ring, pkt, hdr[0]) != hdr[0])
		return false;		/* cannot happen: written together */

	air_begin(hdr[2], hdr[3]);
	midi_ble_packet(hdr[1], pkt, hdr[0]);
	air_end();
	return true;
}

/*
 * The link to the pedal (link.h).  What goes to it waits here until its
 * stream's window allows: the radio's answers whole behind a length byte,
 * and MIDI from the air as records, each the bytes one Bluetooth packet
 * decoded to with the peer it came from and whether to trust it:
 *
 *	peer, flags, length (two bytes, low first), bytes
 *
 * Both queues are written only from the loop.
 */
static struct link radio_link;

/* A run of the link's load test (linktest.h), and counts owed to the pedal */
static struct linktest test;
static bool test_counts_due;

#define AIR_Q_SHIFT	11
#define AIR_Q_SIZE	(1 << AIR_Q_SHIFT)
#define AIR_Q_MASK	(AIR_Q_SIZE - 1)
#define CTL_Q_SHIFT	9
#define CTL_Q_SIZE	(1 << CTL_Q_SHIFT)
#define CTL_Q_MASK	(CTL_Q_SIZE - 1)

static uint8_t air_q[AIR_Q_SIZE];
static uint16_t air_head, air_done, air_tail, air_off;
static uint16_t air_rec;		/* where the open record starts */
static uint8_t ctl_q[CTL_Q_SIZE];
static uint16_t ctl_head, ctl_tail, ctl_sent;
static uint32_t to_pedal_lost;

/*
 * The most one Bluetooth packet can decode to: every byte of it, with a
 * status byte put back in front of each running-status message.
 */
#define AIR_REC_MAX	(4 + 2 * 256)

static bool air_room(void)
{
	return (uint16_t)(AIR_Q_SIZE - (uint16_t)(air_head - air_tail)) >=
	       AIR_REC_MAX;
}

static void air_begin(uint8_t peer, uint8_t flags)
{
	air_rec = air_head;
	air_q[air_head++ & AIR_Q_MASK] = peer;
	air_q[air_head++ & AIR_Q_MASK] = flags;
	air_head += 2;
}

static void air_end(void)
{
	uint16_t len = air_head - air_rec - 4;

	if (!len) {
		air_head = air_rec;
		return;
	}
	air_q[(air_rec + 2) & AIR_Q_MASK] = len & 0xff;
	air_q[(air_rec + 3) & AIR_Q_MASK] = len >> 8;
	air_done = air_head;
}

/* MIDI bytes for the RP2354, from the packet being decoded */
void midi_uart_send(const uint8_t *buf, size_t len)
{
	if (len > (uint16_t)(AIR_Q_SIZE - (uint16_t)(air_head - air_tail))) {
		to_pedal_lost += len;
		return;
	}
	for (size_t i = 0; i < len; i++)
		air_q[air_head++ & AIR_Q_MASK] = buf[i];
}

void midi_uart_control(const uint8_t *msg, size_t len)
{
	uint16_t room = CTL_Q_SIZE - (uint16_t)(ctl_head - ctl_tail);

	if (len > 255 || len + 1 > room) {
		to_pedal_lost += len;
		return;
	}
	ctl_q[ctl_head++ & CTL_Q_MASK] = len;
	for (size_t i = 0; i < len; i++)
		ctl_q[ctl_head++ & CTL_Q_MASK] = msg[i];
}

/*
 * printk's text, a line at a time, for the debug stream.  printk can be
 * called from the Bluetooth stack's thread or from an interrupt as well as
 * from the loop, so the line and the queue are kept under a lock.  A line
 * longer than a packet is cut where the packet ends; one with no room in
 * the queue is dropped and counted.
 */
#define DBG_Q_SHIFT	10
#define DBG_Q_SIZE	(1 << DBG_Q_SHIFT)
#define DBG_Q_MASK	(DBG_Q_SIZE - 1)

static uint8_t dbg_q[DBG_Q_SIZE];
static uint16_t dbg_head, dbg_tail;
static char dbg_line[LINK_PAYLOAD_MAX];
static uint16_t dbg_len;
static uint32_t dbg_lost;
static struct k_spinlock dbg_lock;

static int dbg_putc(int c)
{
	k_spinlock_key_t key = k_spin_lock(&dbg_lock);

	if (c != '\r') {
		dbg_line[dbg_len++] = c;
		if (c == '\n' || dbg_len == sizeof(dbg_line)) {
			uint16_t room = DBG_Q_SIZE -
					(uint16_t)(dbg_head - dbg_tail);

			if (1u + dbg_len <= room) {
				dbg_q[dbg_head++ & DBG_Q_MASK] = dbg_len;
				for (uint16_t i = 0; i < dbg_len; i++)
					dbg_q[dbg_head++ & DBG_Q_MASK] =
						dbg_line[i];
			} else {
				dbg_lost++;
			}
			dbg_len = 0;
		}
	}
	k_spin_unlock(&dbg_lock, key);
	return c;
}

/* The next whole line, if there is one */
static uint16_t dbg_take(uint8_t *out)
{
	k_spinlock_key_t key = k_spin_lock(&dbg_lock);
	uint16_t len = 0;

	if (dbg_head != dbg_tail) {
		len = dbg_q[dbg_tail++ & DBG_Q_MASK];
		for (uint16_t i = 0; i < len; i++)
			out[i] = dbg_q[dbg_tail++ & DBG_Q_MASK];
	}
	k_spin_unlock(&dbg_lock, key);
	return len;
}

static bool tx_room(void)
{
	return ring_buf_space_get(&uart_tx_ring) >= LINK_WIRE_MAX;
}

/*
 * Whatever can go to the pedal now: acknowledgements first, then a packet
 * from each stream that has something and room in its window, in turn.
 */
static void link_out(void)
{
	uint8_t wire[LINK_WIRE_MAX], chunk[LINK_PAYLOAD_MAX];
	bool moved;
	size_t n;

	while (tx_room() && (n = link_pack_ack(&radio_link, wire)))
		ring_buf_put(&uart_tx_ring, wire, n);

	do {
		moved = false;

		if (ctl_head != ctl_tail && tx_room() &&
		    link_can_send(&radio_link, LINK_CONTROL, LINK_ALL)) {
			uint8_t len = ctl_q[ctl_tail & CTL_Q_MASK];
			uint16_t left = len - ctl_sent;
			uint16_t take = left < LINK_PAYLOAD_MAX ? left
							       : LINK_PAYLOAD_MAX;
			uint8_t flags = ctl_sent ? 0 : LINK_FIRST;

			for (uint16_t i = 0; i < take; i++)
				chunk[i] = ctl_q[(ctl_tail + 1 + ctl_sent + i) &
						 CTL_Q_MASK];
			ctl_sent += take;
			if (ctl_sent == len) {
				flags |= LINK_LAST;
				ctl_tail += 1 + len;
				ctl_sent = 0;
			}
			n = link_pack(&radio_link, LINK_CONTROL, LINK_ALL,
				      flags, chunk, take, wire);
			ring_buf_put(&uart_tx_ring, wire, n);
			moved = true;
		}

		if (air_tail != air_done && tx_room()) {
			uint8_t peer = air_q[air_tail & AIR_Q_MASK];
			uint8_t trust = air_q[(air_tail + 1) & AIR_Q_MASK] &
					LINK_TRUSTED;
			uint16_t len = air_q[(air_tail + 2) & AIR_Q_MASK] |
				       air_q[(air_tail + 3) & AIR_Q_MASK] << 8;
			uint16_t left = len - air_off;
			uint16_t take = left < LINK_PAYLOAD_MAX ? left
							       : LINK_PAYLOAD_MAX;

			if (link_can_send(&radio_link, LINK_MIDI, peer)) {
				for (uint16_t i = 0; i < take; i++)
					chunk[i] = air_q[(air_tail + 4 +
							  air_off + i) &
							 AIR_Q_MASK];
				air_off += take;
				if (air_off == len) {
					air_tail += 4 + len;
					air_off = 0;
				}
				n = link_pack(&radio_link, LINK_MIDI, peer,
					      trust | ((chunk[0] & 0x80) &&
						       chunk[0] != 0xF7 ?
						       LINK_FIRST : 0),
					      chunk, take, wire);
				ring_buf_put(&uart_tx_ring, wire, n);
				moved = true;
			}
		}

		if (test_counts_due && tx_room() &&
		    link_can_send(&radio_link, LINK_TEST, LINK_ALL)) {
			size_t len = linktest_counts_pack(&test, chunk);

			n = link_pack(&radio_link, LINK_TEST, LINK_ALL,
				      LINK_FIRST | LINK_LAST, chunk, len, wire);
			ring_buf_put(&uart_tx_ring, wire, n);
			test_counts_due = false;
			moved = true;
		}

		if (tx_room()) {
			size_t len;
			int i = linktest_next(&test, &radio_link, chunk, &len);

			if (i >= 0) {
				n = link_pack(&radio_link, LINK_TEST, i + 1,
					      LINK_FIRST | LINK_LAST, chunk,
					      len, wire);
				ring_buf_put(&uart_tx_ring, wire, n);
				moved = true;
			}
		}

		if (dbg_head != dbg_tail && tx_room() &&
		    link_can_send(&radio_link, LINK_DEBUG, LINK_ALL)) {
			uint16_t len = dbg_take(chunk);

			if (len) {
				n = link_pack(&radio_link, LINK_DEBUG, LINK_ALL,
					      LINK_FIRST | LINK_LAST, chunk,
					      len, wire);
				ring_buf_put(&uart_tx_ring, wire, n);
				moved = true;
			}
		}
	} while (moved);

	tx_kick();
}

/*
 * How much the pedal has sent that has not been dealt with yet, and
 * whether it is being held off.
 *
 * Read by the radio's statistics message.  A backlog that is not
 * draining is what refuses the driver a buffer and deasserts RTS, so
 * between them the two say whether the pedal has been stopped, is
 * keeping up, or is simply not sending.
 */
uint32_t midi_uart_backlog(void)
{
	return ring_buf_size_get(&uart_rx_ring);
}

bool midi_uart_halted(void)
{
	return rx_stopped;
}

uint32_t midi_uart_lost(void)
{
	return rx_lost;
}

/*
 * The link's own counts: packets that never arrived, had nowhere to go, or
 * were malformed, bytes for the pedal there was no room for, and the times
 * the receiver stopped.
 */
void midi_uart_link_counts(uint32_t *gaps, uint32_t *refused, uint32_t *bad,
			   uint32_t *lost, uint32_t *stops)
{
	*gaps = radio_link.gaps;
	*refused = radio_link.refused;
	*bad = radio_link.rx.bad;
	*lost = to_pedal_lost;
	*stops = rx_stops;
}

uint32_t midi_uart_written_off(void)
{
	return radio_link.written_off;
}

uint32_t midi_uart_crc_failed(void)
{
	return radio_link.rx.crc;
}

uint32_t midi_uart_received(void)
{
	return rx_bytes;
}

/*
 * A packet from the pedal that was dropped, as it came off the wire, and
 * where in the byte stream it ended, for finding what the losses have in
 * common.  32 bytes a line, so that a line fits a debug packet.
 */
static void rx_failed(const struct link_rx *rx)
{
	static const char digit[] = "0123456789abcdef";
	char line[3 * 32 + 1];

	printk("link: dropped, %s, %u bytes, ending at byte %u\n",
	       rx->failed == LINK_FAILED_CRC ? "CRC" : "malformed",
	       rx->wire_len, rx_pos);
	for (uint16_t i = 0; i < rx->wire_len; i += 32) {
		uint16_t n = 0;

		for (uint16_t j = i; j < rx->wire_len && j < i + 32; j++) {
			line[n++] = digit[rx->wire[j] >> 4];
			line[n++] = digit[rx->wire[j] & 15];
			line[n++] = ' ';
		}
		line[n - 1] = 0;
		printk("link:   %s\n", line);
	}
}

/*
 * The radio's hello (link.h): a zero first, so that the pedal's decoder is
 * in step, then the boot id that tells a repeat from a new start, and what
 * this image is.  Sent at boot and every HELLO_MS after, until the pedal
 * has sent anything back.
 */
#define HELLO_MS	50

static uint32_t boot_id, hello_at;

static void hello(void)
{
	static const char who[] = "radio " __DATE__ " " __TIME__;
	uint8_t payload[4 + sizeof(who) - 1];
	uint8_t wire[LINK_WIRE_MAX];

	payload[0] = boot_id;
	payload[1] = boot_id >> 8;
	payload[2] = boot_id >> 16;
	payload[3] = boot_id >> 24;
	memcpy(payload + 4, who, sizeof(who) - 1);

	wire[0] = 0;
	ring_buf_put(&uart_tx_ring, wire, 1);
	ring_buf_put(&uart_tx_ring, wire,
		     link_encode(LINK_HELLO, LINK_ALL, 0, LINK_FIRST | LINK_LAST,
				 payload, sizeof(payload), wire));
	tx_kick();
	hello_at = k_uptime_get_32();
}

/*
 * MIDI packets from the pedal, held until they are on the air - at least
 * as many as the windows let the pedal send, so there is always a slot,
 * and a power of two because the indices are masked - and a command
 * being put back together from its packets.
 */
#define HELD_SHIFT	5
#define HELD_SIZE	(1 << HELD_SHIFT)
#define HELD_MASK	(HELD_SIZE - 1)
BUILD_ASSERT(HELD_SIZE >= LINK_WINDOW * LINK_STREAMS);

static struct held {
	uint8_t buf[LINK_PAYLOAD_MAX];
	uint16_t len, pos;
	uint8_t peer, seq;
} in_hand[HELD_SIZE];
static uint8_t hand_head, hand_tail;
static bool heard;		/* the pedal has sent something */

static uint8_t cmd_in[256];
static uint16_t cmd_len;
static bool cmd_ok;

/*
 * A packet from the pedal.  A command is dealt with as soon as its last
 * packet is in; MIDI waits in a slot, and is acknowledged once it has gone.
 */
static void link_in(void)
{
	struct link *l = &radio_link;
	const uint8_t *h = l->rx.buf;
	uint16_t len = l->rx.len - LINK_HEADER;

	if (link_take(l) != LINK_GOT_DATA)
		return;

	switch (h[0]) {
	case LINK_MIDI:
		if ((uint8_t)(hand_head - hand_tail) == HELD_SIZE) {
			l->refused++;
			link_consumed(l);
			break;
		}
		memcpy(in_hand[hand_head & HELD_MASK].buf, h + LINK_HEADER,
		       len);
		in_hand[hand_head & HELD_MASK].len = len;
		in_hand[hand_head & HELD_MASK].pos = 0;
		in_hand[hand_head & HELD_MASK].peer = h[1];
		in_hand[hand_head & HELD_MASK].seq = h[2];
		hand_head++;
		break;

	case LINK_CONTROL:
		if (h[3] & LINK_FIRST) {
			cmd_len = 0;
			cmd_ok = true;
		}
		if (cmd_len + len > sizeof(cmd_in))
			cmd_ok = false;
		else {
			memcpy(cmd_in + cmd_len, h + LINK_HEADER, len);
			cmd_len += len;
		}
		if ((h[3] & LINK_LAST) && cmd_ok)
			midi_radio_command(cmd_in, cmd_len);
		link_consumed(l);
		break;

	case LINK_TEST:
		if (h[1] == LINK_ALL) {
			struct linktest_cfg cfg;

			if (linktest_cfg_unpack(h + LINK_HEADER, len, &cfg)) {
				linktest_start(&test, &cfg, k_uptime_get_32());
				printk("linktest: %u streams of %u, stream %d "
				       "stuck %u ms, %u lines\n", cfg.streams,
				       cfg.count, cfg.stuck == 0xff ? -1 :
				       cfg.stuck, cfg.stuck_ms, cfg.lines);
			} else if (len && h[LINK_HEADER] == LINKTEST_ASK) {
				test_counts_due = true;
			}
		} else {
			uint32_t bad = test.bad;
			bool keep = linktest_got(&test, h[2], h + LINK_HEADER,
						 len, k_uptime_get_32());

			/* Which packet, and where it differs, for the log */
			if (test.bad != bad) {
				uint8_t want[LINK_PAYLOAD_MAX];
				size_t wl = linktest_fill(h[LINK_HEADER],
					h[LINK_HEADER + 1] |
					h[LINK_HEADER + 2] << 8, want);
				size_t j = 0;

				while (j < len && j < wl &&
				       want[j] == h[LINK_HEADER + j])
					j++;
				printk("linktest bad: stream %u cnt %u len %u "
				       "want %u, differs at %u: %02x not %02x\n",
				       h[LINK_HEADER], h[LINK_HEADER + 1] |
				       h[LINK_HEADER + 2] << 8, len, wl, j,
				       j < len ? h[LINK_HEADER + j] : 0,
				       j < wl ? want[j] : 0);
			}
			if (!keep)
				break;		/* held, on the stuck stream */
		}
		link_consumed(l);
		break;

	default:
		link_consumed(l);
		break;
	}
}

/* How often acknowledgements are said again (link.h) */
#define ACK_AGAIN_MS	100

static uint32_t acked_at;

int main(void)
{
	if (!device_is_ready(link))
		return -ENODEV;

	link_init(&radio_link);
	radio_link.up = true;
	__printk_hook_install(dbg_putc);

	uart_callback_set(link, uart_cb, NULL);
	for (int i = 1; i < RX_BUFS; i++)
		rx_free[i] = true;
	uart_rx_enable(link, rx_buf[0], RX_BUF_LEN, RX_TIMEOUT_US);

	boot_id = sys_rand32_get();
	hello();

	midi_ble_start();

	for (;;) {
		uint8_t *buf;
		uint32_t n, took = 0, fed = 0;

		link_tick(&radio_link, k_uptime_get_32());

		link_out();

		/*
		 * Subscription changes go to the pedal before anything a
		 * client asks for.  The pedal throws replies away until it
		 * knows somebody is subscribed, and a client that
		 * subscribes and asks at once has both arrive together:
		 * the notice is queued here, ahead of the request, and
		 * link_out() sends the control stream ahead of MIDI.
		 */
		midi_ble_notices();

		/* What arrived over the air, decoded here rather than in
		 * the callback that received it. */
		while (air_room() && ble_in_drain())
			;

		/*
		 * Everything the pedal has sent, every pass: commands are
		 * dealt with at once, and MIDI waits in its slots, which the
		 * window keeps from overflowing.  A slow Bluetooth client
		 * holds up the MIDI stream and nothing else.
		 */
		n = ring_buf_get_claim(&uart_rx_ring, &buf, UINT32_MAX);
		for (took = 0; took < n; took++) {
			rx_pos++;
			if (link_rx_byte(&radio_link.rx, buf[took])) {
				heard = true;
				link_in();
			} else if (radio_link.rx.failed) {
				rx_failed(&radio_link.rx);
			}
		}
		ring_buf_get_finish(&uart_rx_ring, took);

		/*
		 * The MIDI held, onto the air as fast as it will go.  One
		 * free buffer per byte is enough: a single byte can fill the
		 * packet being built and so send one, and once sent the next
		 * packet has the whole MTU free.
		 */
		while (hand_tail != hand_head && midi_ble_ready()) {
			struct held *h = &in_hand[hand_tail & HELD_MASK];

			midi_ble_to(h->peer);
			while (h->pos < h->len && midi_ble_ready()) {
				midi_ble_feed(h->buf[h->pos++]);
				fed++;
			}
			if (h->pos < h->len)
				break;
			link_done(&radio_link, LINK_MIDI, h->peer, h->seq);
			hand_tail++;
		}

		if (took || fed)
			continue;

		/*
		 * Nothing waiting, so whatever is half-packed is as
		 * complete as it is going to get: send it rather than
		 * hold it for company.  This is what batches a burst into
		 * full packets and still gets a lone note out at once.
		 *
		 * Only when there is somewhere for it to go.  A flush that
		 * cannot send discards, so checking first is the difference
		 * between waiting a pass and losing the packet.
		 */
		if (midi_ble_ready())
			midi_ble_flush();

		/*
		 * And if the ring filled far enough to stop the far end,
		 * it has now drained: start listening again.
		 */
		if (rx_stopped && !rx_starved()) {
			uint8_t *buf = rx_take();

			if (buf) {
				rx_stopped = false;
				uart_rx_enable(link, buf, RX_BUF_LEN,
					       RX_TIMEOUT_US);
			}
		}

		if (!heard && k_uptime_get_32() - hello_at >= HELLO_MS)
			hello();

		linktest_release(&test, &radio_link, k_uptime_get_32());
		if (test.running && test.lines) {
			printk("linktest line %u: the quick brown fox jumps "
			       "over the lazy dog, 0123456789\n", test.lines);
			test.lines--;
		}
		if (k_uptime_get_32() - acked_at >= ACK_AGAIN_MS) {
			link_ack_again(&radio_link);
			acked_at = k_uptime_get_32();
		}

		link_out();
		k_sleep(K_MSEC(1));
	}
}
