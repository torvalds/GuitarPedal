#ifndef MIDI_TX_H
#define MIDI_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//
// What the pedal says, queued rather than sent.
//
// A reply used to be turned into four-byte MIDI packets and pushed at USB
// in one pass of the main loop, with no yield anywhere in it.
// usb_audio_task() is the last thing in that loop, and the audio
// endpoint's software fifo holds three packets - three milliseconds.  A
// reply that takes longer than that to send starves the audio endpoint
// for as long as it runs, and the schema takes hundreds of milliseconds.
//
// Two things were wrong and they are easy to confuse:
//
//  - The *size* of a reply.  Even with the host reading as fast as it
//    can, one pass of the main loop spent sending a whole schema is a
//    pass not spent feeding audio.
//  - Whether anything is *reading*.  usb_midi_write() spins for 20ms per
//    packet on a full transmit fifo.  A host that drains the endpoint
//    keeps that fifo empty and the spin almost never happens; a host that
//    has stopped reading turns every single packet into a timeout, which
//    measured 12-14x worse than the size effect on its own.
//
// A queue answers both.  The reply is serialised once, into here, and
// then handed to USB a few packets at a time from the main loop -
// so a long reply costs many short passes instead of one enormous one,
// and a full fifo means "come back later" instead of a 20ms wait.
//
// It also closes a hole that a resume-where-you-left-off fix would have
// opened.  usb-device.c explains that incoming MIDI is polled from the
// main loop rather than from tud_midi_rx_cb() because a parameter write
// landing in the middle of a sender that is walking effects[] would have
// it report a mixture of before and after.  Serialising the whole reply
// in one pass and draining it afterwards keeps that guarantee by
// construction rather than by convention.
//
// SysEx goes to USB only.  The hardware jacks run at 31250 baud, where
// the schema is several seconds of wire time, and uart_midi_write() is a
// 512-byte ring that drops silently when full - so the TRS side would
// need to solve the slow-consumer problem before it could carry any of
// this, and that is a decision rather than an omission.
//

//
// Sizing.
//
// The payload ring only holds *generated* bytes.  Anything already in
// flash is queued by pointer and costs a descriptor and nothing else,
// which is what keeps the schema out of RAM entirely.
//
// What has to fit is a state dump.  That is 140 bytes as things stand,
// and about 1.4kB in the worst case where every effect is routed and
// every rule table is full.  2kB leaves room for that with an identity
// reply alongside it.
//
// Both sizes are powers of two so that the ring index is a mask.
//
#define MIDI_TX_PAYLOAD	2048
#define MIDI_TX_MSGS	16

// How long a full USB fifo is waited on before the host counts as gone
#define MIDI_TX_STALL_MS 100

//
// One run of bytes waiting to go out.
//
// 'more' means the next entry finishes what this one starts, which is how
// a message can be part flash and part generated without being copied:
// the schema is a generated 'F0 7D 02', twenty-odd thousand static
// bytes, and a generated 'F7'.  Three descriptors, four bytes of RAM.
//
struct midi_msg {
	const uint8_t *flash;	// non-NULL: the bytes are in flash
	uint32_t off;		// otherwise: where in the payload ring
	uint16_t len;
	bool more;
};

//
// Where one consumer has got to.
//
// The cursor is here rather than in the descriptor because there is more
// than one consumer and they do not keep up with each other: USB waits on
// a host reading an endpoint, and the radio waits on a Bluetooth client.
// One cursor in the descriptor would mean neither could fall behind
// without stopping the other, so a USB endpoint nobody reads would stop
// Bluetooth.
//
struct midi_sink {
	uint32_t tail;		// the message it is working on
	uint16_t sent;		// bytes of that message already handed over
};

//
// The queue, as one thing.
//
// Grouped into a structure rather than into a shared name prefix, so that
// what belongs together is held together by the language and not by
// everybody remembering to type 'midi_tx_'.  The accesses read the same
// either way - midi_tx.head against midi_tx_head - and this way there is
// somewhere obvious to put the next field, and no doubt about whether a
// given global is part of this or merely named like it.
//
// Three things live in here and they have different lifetimes:
//
//  - the rings themselves, and the counters saying what is in flight
//  - 'pend', the bytes copied so far that have not become a descriptor
//  - 'txn', where to rewind to if the reply being built does not fit
//
#ifdef NRF54_SWDIO
//
// Defined in nrf54/uart.h, which comes later in the one translation
// unit because it runs only once the radio probe has answered.
// Declared here rather than in midi.h because that file reaches
// usb-device.c as well, where there is no definition to find.
//
static void nrf54_uart_thru(uint8_t byte);
static bool nrf54_uart_ready(void);
#endif

static struct {
	struct midi_msg ring[MIDI_TX_MSGS];
	uint8_t payload[MIDI_TX_PAYLOAD];

	//
	// Free-running counters, masked only where they index.  head -
	// tail is how much is in flight and stays right across the wrap.
	//
	uint32_t head;
	uint32_t pay_head, pay_tail;

	//
	// 'tail' is the oldest message any consumer still needs, so it is
	// what says when a descriptor and its payload can be reused.
	// midi_tx_reclaim() moves it.
	//
	uint32_t tail;

	struct midi_sink usb;
#ifdef NRF54_SWDIO
	struct midi_sink radio;
#endif

	//
	// The transaction being built.  Nothing here is visible to the
	// drain until midi_tx_commit(), so a reply that turns out not to
	// fit leaves no trace of itself.
	//
	uint32_t txn_head;	// descriptor head when it started
	uint32_t txn_pay;	// payload head when it started
	uint32_t pend_off;	// bytes copied but not yet a descriptor
	uint16_t pend_len;
	bool txn_failed;

	//
	// The packetiser's carry, which belongs to the USB sink - the
	// radio takes the bytes as they are.  See midi_tx_to_usb() for
	// why a packet has to outlive the call that built it.
	//
	uint8_t pack[3];
	unsigned int pack_len;
	uint8_t pkt[4];
	bool pkt_ready;
	uint32_t usb_full_since; // when USB began refusing, in ms; 0 if not
} midi_tx;

static inline uint32_t midi_tx_pay_used(void)
{
	return midi_tx.pay_head - midi_tx.pay_tail;
}

static inline uint32_t midi_tx_msgs_used(void)
{
	return midi_tx.head - midi_tx.tail;
}

//
// Start a reply.  Anything half-built from a previous one is discarded:
// a transaction that never committed had already failed, and its bytes
// are not owed to anybody.
//
static void midi_tx_start(void)
{
	midi_tx.txn_head = midi_tx.head;
	midi_tx.txn_pay = midi_tx.pay_head;
	midi_tx.pend_off = midi_tx.pay_head;
	midi_tx.pend_len = 0;
	midi_tx.txn_failed = false;
}

//
// Close whatever has been copied so far into a descriptor of its own.
//
// Called when a static run interrupts the generated bytes, and again at
// commit.  'more' is set on everything here and cleared once at commit,
// because only the last descriptor of a reply knows it is the last.
//
static void midi_tx_flush_pending(void)
{
	struct midi_msg *m;

	if (!midi_tx.pend_len)
		return;
	if (midi_tx_msgs_used() >= MIDI_TX_MSGS) {
		midi_tx.txn_failed = true;
		return;
	}

	m = &midi_tx.ring[midi_tx.head & (MIDI_TX_MSGS - 1)];
	m->flash = NULL;
	m->off = midi_tx.pend_off;
	m->len = midi_tx.pend_len;
	m->more = true;
	midi_tx.head++;

	midi_tx.pend_off = midi_tx.pay_head;
	midi_tx.pend_len = 0;
}

//
// Generated bytes, copied.
//
static void midi_tx_bytes(const uint8_t *buf, size_t len)
{
	if (midi_tx.txn_failed)
		return;
	if (MIDI_TX_PAYLOAD - midi_tx_pay_used() < len) {
		midi_tx.txn_failed = true;
		return;
	}

	for (size_t i = 0; i < len; i++) {
		midi_tx.payload[midi_tx.pay_head & (MIDI_TX_PAYLOAD - 1)] = buf[i];
		midi_tx.pay_head++;
	}
	midi_tx.pend_len += len;
}

//
// Bytes that are already somewhere permanent, queued where they lie.
//
// The caller is promising the bytes outlive the queue, which for a
// 'static const' in flash is trivially true and for anything else is not.
//
static void midi_tx_static(const uint8_t *buf, size_t len)
{
	struct midi_msg *m;

	if (midi_tx.txn_failed)
		return;

	midi_tx_flush_pending();
	if (midi_tx.txn_failed)
		return;
	if (midi_tx_msgs_used() >= MIDI_TX_MSGS) {
		midi_tx.txn_failed = true;
		return;
	}

	m = &midi_tx.ring[midi_tx.head & (MIDI_TX_MSGS - 1)];
	m->flash = buf;
	m->off = 0;
	m->len = len;
	m->more = true;
	midi_tx.head++;
}

//
// Publish, or leave no trace.
//
// Returning false is not an error the caller has to report - it means
// "not now".  A sender that leaves its request flag set will be back next
// time round the main loop, which is the whole of the backpressure and is
// also why several requests for the same reply still collapse into one.
//
static bool midi_tx_commit(void)
{
	midi_tx_flush_pending();

	if (midi_tx.txn_failed || midi_tx.head == midi_tx.txn_head) {
		// Rewind. Nothing committed, so nothing is owed.
		midi_tx.head = midi_tx.txn_head;
		midi_tx.pay_head = midi_tx.txn_pay;
		midi_tx.pend_len = 0;
		return false;
	}

	// Only the last descriptor is the end of the message.
	midi_tx.ring[(midi_tx.head - 1) & (MIDI_TX_MSGS - 1)].more = false;
	return true;
}

//
// The packetiser, and the one packet it may be holding.
//
// MIDI 1.0 carries SysEx three bytes at a time: CIN 0x04 while the stream
// continues, and 0x05, 0x06 or 0x07 for a packet that ends it with one,
// two or three bytes in it.  Which means the shape of a packet is not
// known until its last byte has been read, so the accumulator has to
// survive both the end of a descriptor and the end of a drain.
//
// A built packet that USB would not take stays here rather than being
// rebuilt, because the bytes behind it have already been consumed.
//
static uint8_t midi_tx_byte(const struct midi_msg *m, uint16_t i)
{
	if (m->flash)
		return m->flash[i];
	return midi_tx.payload[(m->off + i) & (MIDI_TX_PAYLOAD - 1)];
}

//
// One packet to USB, or given up on.
//
// A host that is mounted and not reading - a computer the pedal only
// draws power from - fills the fifo and never empties it, and nothing
// says so.  Everything waiting on USB would wait for ever, Bluetooth
// included, so a fifo that has refused for MIDI_TX_STALL_MS straight
// counts as a host that has stopped reading and the packet is dropped.
// A host that reads frees room every millisecond.
//
// True when the packet is gone, taken or dropped.
//
bool usb_midi_write_or_drop(const uint8_t packet[4])
{
	uint32_t now = to_ms_since_boot(get_absolute_time());

	if (usb_midi_write_nb(packet)) {
		midi_tx.usb_full_since = 0;
		return true;
	}
	if (!midi_tx.usb_full_since) {
		midi_tx.usb_full_since = now | 1;
		return false;
	}
	return now - midi_tx.usb_full_since >= MIDI_TX_STALL_MS;
}

static bool midi_tx_push(void)
{
	if (!midi_tx.pkt_ready)
		return true;

	if (!usb_midi_write_or_drop(midi_tx.pkt))
		return false;

	midi_tx.pkt_ready = false;
	return true;
}

//
// How much to hand over in one pass.
//
// "Until the endpoint is full" is the obvious rule and it is the wrong
// one, because CFG_TUD_MIDI_TX_BUFSIZE is 8192 - most of a schema.  A
// drain that stops only when the fifo is full very nearly sends the whole
// reply in one pass, which is the thing this queue exists to stop, and it
// measured almost exactly that: no better than before.
//
// What matters is not what the fifo will hold but what USB will actually
// move before the main loop comes round again.  A full-speed bulk
// endpoint is 64 bytes a frame, so sixteen four-byte packets a
// millisecond, and the audio endpoint has three milliseconds of slack in
// front of it.  One frame's worth per pass keeps the wire busy and leaves
// the loop immediately - and since the loop goes round far faster than a
// millisecond, the wire stays saturated anyway.
//
#define MIDI_TX_PER_PASS 16

//
// Hand USB a frame's worth, and no more.
//
// It returns on the packet budget or on a full endpoint, whichever comes
// first, which is what keeps usb_audio_task() running on time no matter
// how long the reply is.  Returning early leaves this sink where it is
// and costs no other consumer anything.
//
static void midi_tx_to_usb(void)
{
	struct midi_sink *s = &midi_tx.usb;
	unsigned int pushed = 0;

	while (s->tail != midi_tx.head) {
		struct midi_msg *m = &midi_tx.ring[s->tail & (MIDI_TX_MSGS - 1)];

		if (midi_tx.pkt_ready) {
			if (pushed >= MIDI_TX_PER_PASS)
				return;
			if (!midi_tx_push())
				return;
			pushed++;
		}

		if (s->sent >= m->len) {
			s->tail++;
			s->sent = 0;
			continue;
		}

		uint8_t b = midi_tx_byte(m, s->sent++);

		if (b == 0xF0)
			midi_tx.pack_len = 0;
		midi_tx.pack[midi_tx.pack_len++] = b;

		if (b == 0xF7) {
			midi_tx.pkt[0] = 0x04 + midi_tx.pack_len;
			midi_tx.pkt[1] = midi_tx.pack[0];
			midi_tx.pkt[2] = midi_tx.pack_len > 1 ? midi_tx.pack[1] : 0;
			midi_tx.pkt[3] = midi_tx.pack_len > 2 ? midi_tx.pack[2] : 0;
			midi_tx.pack_len = 0;
			midi_tx.pkt_ready = true;
		} else if (midi_tx.pack_len == 3) {
			midi_tx.pkt[0] = 0x04;
			midi_tx.pkt[1] = midi_tx.pack[0];
			midi_tx.pkt[2] = midi_tx.pack[1];
			midi_tx.pkt[3] = midi_tx.pack[2];
			midi_tx.pack_len = 0;
			midi_tx.pkt_ready = true;
		}
	}

	if (pushed < MIDI_TX_PER_PASS)
		midi_tx_push();
}

//
// Channel messages for the radio: the CCs, notes and pitch bends that
// midi.h's senders write straight to USB and to the jacks.
//
// USB takes each as a packet of its own, which can go between two of a
// SysEx reply's.  The radio takes a byte stream, where a status byte in
// the middle of a SysEx message ends it - so these wait here, and go
// between replies.  A message that does not fit is dropped, as the
// jacks' ring drops one.
//
#define MIDI_TX_SHORT	256

#ifdef NRF54_SWDIO
static struct {
	uint8_t buf[MIDI_TX_SHORT];
	uint32_t head, tail;
	bool mid;			// a SysEx reply is part sent
} midi_short;
#endif

void radio_midi_write(const uint8_t packet[4])
{
#ifdef NRF54_SWDIO
	int len = midi_cin_length(packet[0] & 0x0F);

	if (MIDI_TX_SHORT - (midi_short.head - midi_short.tail) < (uint32_t)len)
		return;
	for (int i = 0; i < len; i++)
		midi_short.buf[midi_short.head++ & (MIDI_TX_SHORT - 1)] =
			packet[1 + i];
#endif
}

#ifdef NRF54_SWDIO
//
// Send the radio the channel messages that are waiting; false if it could
// not take them all.  A message is never left half sent, because what
// follows could be a reply.
//
static bool midi_tx_short_to_radio(void)
{
	while (midi_short.tail != midi_short.head) {
		uint8_t status = midi_short.buf[midi_short.tail & (MIDI_TX_SHORT - 1)];
		int len = midi_cin_length(midi_status_cin(status));

		if (!nrf54_uart.listening) {
			midi_short.tail = midi_short.head;
			break;
		}
		// Ready is under half full, far more room than three bytes
		if (!nrf54_uart_ready())
			return false;
		while (len--)
			nrf54_uart_thru(midi_short.buf[midi_short.tail++ &
						       (MIDI_TX_SHORT - 1)]);
	}
	return true;
}

//
// The same bytes to the radio, unpacketised.
//
// It stops when the link's queue is half full, which bounds a pass to
// about 512 bytes without needing a budget of its own, and leaves the
// rest for the next one.  A board with no radio answers ready and
// discards, so this sink still moves and the queue is still reclaimed.
//
static void midi_tx_to_radio(void)
{
	struct midi_sink *s = &midi_tx.radio;

	for (;;) {
		if (!midi_short.mid && !midi_tx_short_to_radio())
			return;
		if (s->tail == midi_tx.head)
			return;

		struct midi_msg *m = &midi_tx.ring[s->tail & (MIDI_TX_MSGS - 1)];

		if (s->sent >= m->len) {
			midi_short.mid = m->more;
			s->tail++;
			s->sent = 0;
			continue;
		}

		//
		// Nothing on the far side has asked to be sent anything,
		// so throw it away here rather than spend the link on it.
		// The radio says when that changes; until it has said so
		// the answer is no.
		//
		// Thrown away rather than waited for, because waiting
		// holds the descriptor, and a descriptor held long enough
		// stops midi_tx_commit() taking new replies - which would
		// make a Bluetooth client that never subscribes stop the
		// USB one too.  The same reason midi_tx_push() discards
		// for a host that is not mounted.
		//
		if (!nrf54_uart.listening) {
			s->sent = m->len;
			continue;
		}

		if (!nrf54_uart_ready())
			return;

		midi_short.mid = true;
		nrf54_uart_thru(midi_tx_byte(m, s->sent++));
	}
}
#endif

//
// Release what every consumer has finished with.
//
// A descriptor and the payload behind it belong to the slowest sink, so
// nothing is reused until all of them are past it.  That is what makes a
// stalled consumer cost queue space rather than costing the other
// consumers their progress: midi_tx_commit() starts refusing new replies,
// which is backpressure, and it says so by returning false.
//
static void midi_tx_reclaim(void)
{
	while (midi_tx.tail != midi_tx.head) {
		struct midi_msg *m = &midi_tx.ring[midi_tx.tail & (MIDI_TX_MSGS - 1)];

		if (midi_tx.usb.tail == midi_tx.tail)
			return;
#ifdef NRF54_SWDIO
		if (midi_tx.radio.tail == midi_tx.tail)
			return;
#endif
		if (!m->flash)
			midi_tx.pay_tail += m->len;
		midi_tx.tail++;
	}
}

//
// One pass of everything that is waiting to go out.
//
static void midi_tx_drain(void)
{
	midi_tx_to_usb();
#ifdef NRF54_SWDIO
	midi_tx_to_radio();
#endif
	midi_tx_reclaim();
}

//
// Is there a reply in flight?
//
// Asked by a sender that would rather wait than queue a second one behind
// the first, and by anything that wants to know the pedal has finished
// talking.
//
static inline bool midi_tx_busy(void)
{
	return midi_tx.head != midi_tx.tail;
}

#endif
