/*
 * The UART side of the bridge: uart30 carries the link (link.h) to and
 * from the RP2354.  midi.c is the Bluetooth side.
 *
 * It carries nothing else.  There is no console anywhere on this radio -
 * prj.conf says why.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/random/random.h>
#include <hal/nrf_uarte.h>

#include "midi.h"
#include "link.h"
#include "linktest.h"
#include "uartrx.h"

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

/*
 * uart30, driven from the loop rather than by Zephyr's driver: one ring
 * each way and no interrupt.  The devicetree says which pins; uart_start()
 * sets the rest.
 *
 * There is no RTS/CTS.  The receiver never stops, so RTS would never be
 * raised, and the link's windows already pace both sides.
 */
#define LINK_UART	DT_CHOSEN(pedal_midi_uart)

PINCTRL_DT_DEFINE(LINK_UART);

static NRF_UARTE_Type *const uarte = (NRF_UARTE_Type *)DT_REG_ADDR(LINK_UART);

/* Receive is one ring, refilled by the DMA for ever (uartrx.h) */
static uint8_t rx_ring[UARTRX_SIZE];
static struct uartrx uart_rx;

/*
 * Transmit is a ring the loop fills, sent by the DMA from the tail to the
 * head or the end of the ring, whichever comes first.
 */
#define TX_SHIFT	10
#define TX_SIZE		(1 << TX_SHIFT)
#define TX_MASK		(TX_SIZE - 1)

static uint8_t tx_ring[TX_SIZE];
static uint16_t tx_head, tx_tail;
static uint16_t tx_len;		/* bytes the DMA has, 0 when idle */

static void uart_start(void)
{
	pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(LINK_UART),
			    PINCTRL_STATE_DEFAULT);

	/* 8N1, no RTS/CTS, is the reset value */
	BUILD_ASSERT(DT_PROP(LINK_UART, current_speed) == 1000000);
	nrf_uarte_baudrate_set(uarte, NRF_UARTE_BAUDRATE_1000000);
	nrf_uarte_enable(uarte);

	uarte->DMA.RX.MATCH.CANDIDATE[0] = 0;
	uarte->DMA.RX.MATCH.CONFIG = UARTE_DMA_RX_MATCH_CONFIG_ENABLE0_Msk;
	nrf_uarte_rx_buffer_set(uarte, rx_ring, UARTRX_SIZE);
	nrf_uarte_shorts_enable(uarte, NRF_UARTE_SHORT_ENDRX_STARTRX);
	nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STARTRX);
}

/* END is read on both sides of AMOUNT, so that the two are from one pass */
static void rx_written(void)
{
	uint32_t end, amount;

	do {
		end = uarte->EVENTS_DMA.RX.END;
		amount = uarte->DMA.RX.AMOUNT;
	} while (uarte->EVENTS_DMA.RX.END != end);
	if (end)
		uarte->EVENTS_DMA.RX.END = 0;
	uartrx_written(&uart_rx, end, amount);
	__DMB();
}

static bool tx_room(void)
{
	return TX_SIZE - (uint16_t)(tx_head - tx_tail) >= LINK_WIRE_MAX;
}

static void tx_put(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		tx_ring[tx_head++ & TX_MASK] = buf[i];
}

static void tx_kick(void)
{
	uint16_t len, to_end;

	if (tx_len) {
		if (!uarte->EVENTS_DMA.TX.END)
			return;
		uarte->EVENTS_DMA.TX.END = 0;
		__DMB();
		tx_tail += tx_len;
		tx_len = 0;
	}

	len = tx_head - tx_tail;
	to_end = TX_SIZE - (tx_tail & TX_MASK);
	if (len > to_end)
		len = to_end;
	if (!len)
		return;

	tx_len = len;
	__DMB();
	nrf_uarte_tx_buffer_set(uarte, tx_ring + (tx_tail & TX_MASK), len);
	nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STARTTX);
}

/*
 * Packets that arrived over Bluetooth, waiting for the loop.
 *
 * A GATT callback runs in the Bluetooth stack's own thread, so it copies
 * the packet in here and returns rather than decoding it and writing to
 * the UART.  The RP2354 side follows the same rule, for the same reason:
 * see "Handle incoming MIDI from the main loop, not from a callback".
 *
 * Each is its length, source, peer and flags, then the packet.  A packet
 * is at most the ATT MTU less three, which is under 256.  The stack's
 * thread writes and the loop reads, and both hold the lock to do it.
 *
 * It does not need to be big.  The controller can only hold
 * BT_MAX_CONN + BT_BUF_ACL_RX_COUNT_EXTRA buffers before the host stops
 * it, the largest message the pedal is ever sent is a full rule table at
 * 96 bytes, and there is no bulk inbound direction at all.
 */
#define BLE_IN_SHIFT	10
#define BLE_IN_SIZE	(1 << BLE_IN_SHIFT)
#define BLE_IN_MASK	(BLE_IN_SIZE - 1)

static uint8_t ble_in[BLE_IN_SIZE];
static uint16_t ble_in_head, ble_in_tail;
static uint32_t ble_in_dropped;
static struct k_spinlock ble_in_lock;

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
	k_spinlock_key_t key;

	if (!len || len > 255) {
		ble_in_dropped++;
		return;
	}

	key = k_spin_lock(&ble_in_lock);
	if (BLE_IN_SIZE - (uint16_t)(ble_in_head - ble_in_tail) <
	    sizeof(hdr) + len) {
		ble_in_dropped++;
	} else {
		for (int i = 0; i < sizeof(hdr); i++)
			ble_in[ble_in_head++ & BLE_IN_MASK] = hdr[i];
		for (uint16_t i = 0; i < len; i++)
			ble_in[ble_in_head++ & BLE_IN_MASK] = buf[i];
	}
	k_spin_unlock(&ble_in_lock, key);
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
	k_spinlock_key_t key = k_spin_lock(&ble_in_lock);
	bool any = ble_in_head != ble_in_tail;

	if (any) {
		for (int i = 0; i < sizeof(hdr); i++)
			hdr[i] = ble_in[ble_in_tail++ & BLE_IN_MASK];
		for (uint16_t i = 0; i < hdr[0]; i++)
			pkt[i] = ble_in[ble_in_tail++ & BLE_IN_MASK];
	}
	k_spin_unlock(&ble_in_lock, key);
	if (!any)
		return false;

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
		tx_put(wire, n);

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
			tx_put(wire, n);
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
				tx_put(wire, n);
				moved = true;
			}
		}

		if (test_counts_due && tx_room() &&
		    link_can_send(&radio_link, LINK_TEST, LINK_ALL)) {
			size_t len = linktest_counts_pack(&test, chunk);

			n = link_pack(&radio_link, LINK_TEST, LINK_ALL,
				      LINK_FIRST | LINK_LAST, chunk, len, wire);
			tx_put(wire, n);
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
				tx_put(wire, n);
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
				tx_put(wire, n);
				moved = true;
			}
		}
	} while (moved);

	tx_kick();
}

/* How much the pedal has sent that has not been dealt with yet */
uint32_t midi_uart_backlog(void)
{
	return uart_rx.head - uart_rx.tail;
}

uint32_t midi_uart_lost(void)
{
	return uart_rx.lost;
}

/*
 * The link's own counts: packets that never arrived, had nowhere to go, or
 * were malformed, and bytes for the pedal there was no room for.
 */
void midi_uart_link_counts(uint32_t *gaps, uint32_t *refused, uint32_t *bad,
			   uint32_t *lost)
{
	*gaps = radio_link.gaps;
	*refused = radio_link.refused;
	*bad = radio_link.rx.bad;
	*lost = to_pedal_lost;
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
	return uart_rx.head;
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
	       rx->wire_len, uart_rx.tail);
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
	tx_put(wire, 1);
	tx_put(wire,
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
	link_init(&radio_link);
	radio_link.up = true;
	__printk_hook_install(dbg_putc);

	uart_start();

	boot_id = sys_rand32_get();
	hello();

	midi_ble_start();

	for (;;) {
		uint32_t took = 0, fed = 0;

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
		 *
		 * After an overrun the packet the decoder was in the middle of
		 * is skipped too.
		 */
		rx_written();
		if (uartrx_overrun(&uart_rx))
			radio_link.rx.synced = false;
		while (uart_rx.tail != uart_rx.head) {
			uint8_t b = rx_ring[uart_rx.tail++ & UARTRX_MASK];

			took++;
			if (link_rx_byte(&radio_link.rx, b)) {
				heard = true;
				link_in();
			} else if (radio_link.rx.failed) {
				rx_failed(&radio_link.rx);
			}
		}

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
