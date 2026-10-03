//
// Check the packet link between the pedal and its radio,
// nRF54/app/src/link.h, which both sides compile.
//
// The encoder and the decoder are each other's only test on the board, so
// a mistake that both make the same way would pass there.  Here they are
// checked against packets known in advance, including the cases the board
// only meets on a reset: a receiver starting in the middle of a packet, and
// zeros between packets, and against damage: every byte changed, and every
// byte left out.  Then two ends are joined by a wire in memory, to check the
// part that replaces hardware flow control: per-stream windows,
// acknowledgements, a lost packet, and a hello.
//
// Build and run with 'make test-link && ./test-link'.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nRF54/app/src/link.h"

#define PACKETS	1000

static int fails;
static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL %-44s got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static uint8_t wire[PACKETS * LINK_WIRE_MAX + 1];
static uint8_t want[PACKETS][LINK_FRAME_MAX];
static size_t want_len[PACKETS];

static void coding(void)
{
	struct link_rx rx;
	uint8_t payload[LINK_PAYLOAD_MAX];
	size_t n = 0;
	int got = 0, wrong = 0, inside = 0, longest = 0;

	// Random payloads of every length, a third of their bytes zero
	srand(1);
	wire[n++] = 0;
	for (int k = 0; k < PACKETS; k++) {
		size_t len = rand() % (LINK_PAYLOAD_MAX + 1), e;
		uint8_t h[LINK_HEADER] = { k & 3, k, k * 7, k % 5 ? 0 : 3 };

		for (size_t i = 0; i < len; i++)
			payload[i] = rand() % 3 ? rand() : 0;
		memcpy(want[k], h, LINK_HEADER);
		memcpy(want[k] + LINK_HEADER, payload, len);
		want_len[k] = LINK_HEADER + len;

		e = link_encode(h[0], h[1], h[2], h[3], payload, len, wire + n);
		for (size_t i = n; i < n + e - 1; i++)
			inside += !wire[i];
		if ((int)e > longest)
			longest = e;
		n += e;
	}
	chk("zeros inside packets", inside, 0);
	chk("longest packet within LINK_WIRE_MAX", longest <= LINK_WIRE_MAX, 1);

	link_rx_init(&rx);
	for (size_t i = 0; i < n; i++) {
		if (!link_rx_byte(&rx, wire[i]))
			continue;
		if (rx.len != want_len[got] ||
		    memcmp(rx.buf, want[got], rx.len))
			wrong++;
		got++;
	}
	chk("packets decoded", got, PACKETS);
	chk("packets decoded wrong", wrong, 0);
	chk("packets counted bad", rx.bad, 0);

	// Starting mid-packet loses that packet and nothing else
	link_rx_init(&rx);
	got = 0;
	for (size_t i = 5; i < n; i++)
		got += link_rx_byte(&rx, wire[i]);
	chk("decoded after starting mid-packet", got, PACKETS - 1);

	// Extra zeros, as a side that resets sends, deliver nothing twice
	{
		uint8_t w[2 * LINK_WIRE_MAX + 4], q[3] = { 1, 2, 3 };
		size_t m = 0;

		w[m++] = 0;
		m += link_encode(LINK_MIDI, 7, 0, 0, q, 3, w + m);
		w[m++] = 0;
		w[m++] = 0;
		m += link_encode(LINK_MIDI, 8, 0, 0, q, 3, w + m);
		link_rx_init(&rx);
		got = 0;
		for (size_t i = 0; i < m; i++)
			got += link_rx_byte(&rx, w[i]);
		chk("decoded with extra zeros between", got, 2);
	}
}

//
// A packet with one data byte changed is never delivered: an error within
// eight bits is one a CRC-8 always catches.  A changed COBS code byte or a
// missing byte moves the rest of the packet about, which COBS catches most
// of and an 8-bit CRC catches all but about 1 in 256 of what is left.
//
static void damage(void)
{
	struct link_rx rx;
	uint8_t p[LINK_PAYLOAD_MAX], w[LINK_WIRE_MAX + 1], d[LINK_WIRE_MAX + 1];
	const uint8_t check[] = "123456789";
	uint8_t crc = 0;
	long tried = 0, delivered = 0, uncounted = 0, moved = 0, moved_through = 0;

	for (size_t i = 0; i < 9; i++)
		crc = link_crc8(crc, check[i]);
	chk("CRC-8 of \"123456789\"", crc, 0xf4);

	srand(2);
	for (int k = 0; k < 200; k++) {
		size_t len = rand() % (LINK_PAYLOAD_MAX + 1), n;

		for (size_t i = 0; i < len; i++)
			p[i] = rand() % 3 ? rand() : 0;
		w[0] = 0;
		n = 1 + link_encode(k & 7, k, k, 0, p, len, w + 1);

		// Which bytes are COBS codes rather than data
		bool code[LINK_WIRE_MAX + 1] = { 0 };

		for (size_t at = 1; at < n - 1; at += w[at])
			code[at] = true;

		// Each byte but the zeros at the ends, changed to every value
		for (size_t at = 1; at < n - 1; at++)
			for (int v = 1; v < 256; v++) {
				int got = 0;

				if (v == w[at])
					continue;
				memcpy(d, w, n);
				d[at] = v;
				link_rx_init(&rx);
				for (size_t i = 0; i < n; i++)
					got += link_rx_byte(&rx, d[i]);
				if (code[at]) {
					moved++;
					moved_through += got;
				} else {
					tried++;
					delivered += got;
				}
				uncounted += got == 0 && rx.bad + rx.crc == 0;
			}

		// Each byte left out
		for (size_t at = 1; at < n - 1; at++) {
			int got = 0;

			memcpy(d, w, at);
			memcpy(d + at, w + at + 1, n - at - 1);
			link_rx_init(&rx);
			for (size_t i = 0; i < n - 1; i++)
				got += link_rx_byte(&rx, d[i]);
			moved++;
			moved_through += got;
			uncounted += got == 0 && rx.bad + rx.crc == 0;
		}
	}
	chk("packets with a data byte changed delivered", delivered, 0);
	chk("damaged packets not counted", uncounted, 0);
	chk("other damage through more than 1 in 200",
	    moved_through * 200 > moved, 0);
	printf("test-link: %ld data bytes changed, all caught; %ld codes "
	       "changed or bytes missing, %ld through\n", tried, moved,
	       moved_through);
}

//
// Two ends joined in memory.  deliver() runs every byte of a packet through
// the receiver and returns what link_take() made of it.
//
static int deliver(struct link *to, const uint8_t *w, size_t n)
{
	int got = LINK_GOT_NOTHING;

	for (size_t i = 0; i < n; i++)
		if (link_rx_byte(&to->rx, w[i]))
			got = link_take(to);
	return got;
}

//
// Move a side's clock on the way its loop would, a millisecond at a time:
// link_tick() takes a jump as the loop having been held up.
//
static void tick(struct link *l, uint32_t to)
{
	while ((int32_t)(to - l->now) > 0)
		link_tick(l, l->now + 1);
}

// Send whatever ack 'from' owes to 'to'
static void ack(struct link *from, struct link *to)
{
	uint8_t w[LINK_WIRE_MAX];
	size_t n;

	while ((n = link_pack_ack(from, w)))
		deliver(to, w, n);
}

static void flow(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], hello[LINK_WIRE_MAX], p[1] = { 0x90 };
	const uint8_t boot1[4] = { 1, 2, 3, 4 }, boot2[4] = { 5, 6, 7, 8 };
	size_t n, hn;
	int sent;

	link_init(&pedal);
	link_init(&radio);

	// The pedal believes nothing, and sends nothing, before a hello
	hn = link_encode(LINK_HELLO, 0, 0, 0, boot1, 4, hello + 1);
	hello[0] = 0;
	chk("hello reported", deliver(&pedal, hello, hn + 1), LINK_GOT_HELLO);
	chk("up after hello", pedal.up, 1);
	deliver(&radio, (const uint8_t[]){ 0 }, 1);

	// A window of LINK_WINDOW, and no more, until something is consumed
	for (sent = 0; link_can_send(&pedal, LINK_MIDI, LINK_ALL); sent++) {
		n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
		chk("data arrives", deliver(&radio, w, n), LINK_GOT_DATA);
	}
	chk("packets sent into an unacked window", sent, LINK_WINDOW);

	// Another stream is not held up by the full one
	chk("control open while MIDI is full",
	    link_can_send(&pedal, LINK_CONTROL, LINK_ALL), 1);

	// Received but not consumed: still no room
	ack(&radio, &pedal);
	chk("window still shut before consumed",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);

	// Consuming the last one acknowledges both
	link_consumed(&radio);
	ack(&radio, &pedal);
	chk("window open after the ack",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);

	// An ack from the past, or a repeated one, changes nothing
	n = link_encode(LINK_ACK, LINK_ALL, 0, LINK_MIDI, NULL, 0, w);
	deliver(&pedal, w, n);
	chk("old ack ignored", link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);
	n = link_encode(LINK_ACK, LINK_ALL, 200, LINK_MIDI, NULL, 0, w);
	deliver(&pedal, w, n);
	chk("ack beyond what was sent ignored",
	    pedal.s[0].tx_acked, LINK_WINDOW);

	// A lost packet: counted, and the stream waits for a message start
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	(void)n;			// lost on the way
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, 0, p, 1, w);
	chk("continuation after a loss dropped",
	    deliver(&radio, w, n), LINK_GOT_NOTHING);
	chk("the loss counted", radio.gaps, 1);
	ack(&radio, &pedal);
	chk("dropped packet still acknowledged",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	chk("next message start delivered",
	    deliver(&radio, w, n), LINK_GOT_DATA);
	chk("no second gap", radio.gaps, 1);

	// A lost last ack, with the window full, is made good by a repeat
	while (link_can_send(&pedal, LINK_MIDI, LINK_ALL)) {
		n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
		deliver(&radio, w, n);
		link_consumed(&radio);
	}
	n = link_pack_ack(&radio, w);		// lost on the way
	chk("window full after the lost ack",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("window open after the repeated ack",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);

	// The same hello again is a repeat, and changes nothing
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	chk("repeated hello ignored",
	    deliver(&pedal, hello, hn + 1), LINK_GOT_NOTHING);
	chk("window not reset by a repeat",
	    (uint8_t)(pedal.s[0].tx_next - pedal.s[0].tx_acked), 1);

	// A new boot forgets every stream: the next packet is number 0 again
	hn = link_encode(LINK_HELLO, 0, 0, 0, boot2, 4, hello + 1);
	chk("new boot reported", deliver(&pedal, hello, hn + 1),
	    LINK_GOT_HELLO);
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	{
		struct link_rx rx;

		link_rx_init(&rx);
		link_rx_byte(&rx, 0);
		for (size_t i = 0; i < n; i++)
			link_rx_byte(&rx, w[i]);
		chk("seq restarts after a hello", rx.buf[2], 0);
	}
	chk("window empty after a hello",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);
}

//
// Peers come and go, each with a stream: a busy stream must not be evicted
// while there is room, and when there is not, the oldest goes.
//
static void eviction(void)
{
	struct link radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;
	int got;

	link_init(&radio);
	radio.up = true;

	// The pedal's MIDI to the radio, in steady use
	n = link_encode(LINK_MIDI, LINK_ALL, 0, LINK_FIRST, p, 1, w);
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&radio, w, n);
	link_consumed(&radio);
	link_pack_ack(&radio, w);

	// More peers than there are slots, each sending once
	for (int peer = 1; peer < 3 * LINK_STREAMS; peer++) {
		link_can_send(&radio, LINK_MIDI, peer);
		// ...while the pedal's stream keeps going
		n = link_encode(LINK_MIDI, LINK_ALL, peer, LINK_FIRST, p, 1, w);
		got = deliver(&radio, w, n);
		if (got != LINK_GOT_DATA)
			break;
		link_consumed(&radio);
		link_pack_ack(&radio, w);
	}
	chk("a stream in use survives peers coming and going", radio.gaps, 0);
}

//
// Every packet in a window lost on the wire: nothing newer can arrive to be
// acknowledged, so the sender has to write them off once they cannot still
// be on the way.  A receiver that is only slow must not be written off.
//
static void loss(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	// One packet through, consumed, acknowledged
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	deliver(&radio, w, n);
	link_consumed(&radio);
	ack(&radio, &pedal);

	// The whole window lost
	while (link_can_send(&pedal, LINK_MIDI, LINK_ALL))
		link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);

	// Too soon to say: a repeated ack leaves the window shut
	tick(&pedal, LINK_LOST_MS - 1);
	tick(&radio, LINK_LOST_MS - 1);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("window shut while the packets could be on the way",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);

	// Late enough: written off, and the window opens
	tick(&pedal, LINK_LOST_MS);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("window open once the lost packets are written off",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);
	chk("written off", pedal.written_off, LINK_WINDOW);

	// The next packet arrives as a gap, and is delivered
	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	chk("next packet delivered after the loss",
	    deliver(&radio, w, n), LINK_GOT_DATA);
	chk("loss seen as a gap", radio.gaps, 1);

	// A slow receiver: everything received, nothing consumed, never
	// written off however long it takes
	link_consumed(&radio);
	ack(&radio, &pedal);
	while (link_can_send(&pedal, LINK_MIDI, LINK_ALL)) {
		n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
		deliver(&radio, w, n);
	}
	tick(&pedal, 10 * LINK_LOST_MS);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("a slow receiver keeps its window shut",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);
	ack(&pedal, &radio);			// asks, too
	ack(&radio, &pedal);
	chk("...when asked as well",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);
}

//
// A stream whose first window is lost entirely: the receiver has never heard
// of it, so it has nothing to repeat, and the sender has to ask.
//
static void lost_start(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	while (link_can_send(&pedal, LINK_MIDI, 5))
		link_pack(&pedal, LINK_MIDI, 5, LINK_FIRST, p, 1, w);

	// No ask before the packets could have arrived
	tick(&pedal, LINK_LOST_MS - 1);
	tick(&radio, LINK_LOST_MS - 1);
	chk("no ask too soon", link_pack_ack(&pedal, w) > 0, 0);

	tick(&pedal, LINK_LOST_MS);
	tick(&radio, LINK_LOST_MS);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("a repeat alone does not open it",
	    link_can_send(&pedal, LINK_MIDI, 5), 0);
	ack(&pedal, &radio);
	ack(&radio, &pedal);
	chk("the ask opens it", link_can_send(&pedal, LINK_MIDI, 5), 1);
	chk("the first window written off", pedal.written_off, LINK_WINDOW);
	chk("asked only once", link_pack_ack(&pedal, w) > 0, 0);

	n = link_pack(&pedal, LINK_MIDI, 5, LINK_FIRST, p, 1, w);
	chk("next packet delivered", deliver(&radio, w, n), LINK_GOT_DATA);
	chk("seen as a gap", radio.gaps, 1);
}

//
// The sequence numbers wrap with every ack at the wrap lost: the receiver's
// counts are both 0 again, which link_ack_again() does not repeat.
//
static void lost_wrap(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;
	int sent = 0;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	while (sent < 256) {
		n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
		deliver(&radio, w, n);
		link_consumed(&radio);
		if (++sent < 256 - LINK_WINDOW + 1)
			ack(&radio, &pedal);
		else
			link_pack_ack(&radio, w);	// lost on the way
	}
	chk("window full at the wrap",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 0);
	chk("receiver's counts both 0", radio.s[0].rx_next | radio.s[0].rx_done,
	    0);

	tick(&pedal, LINK_LOST_MS);
	tick(&radio, LINK_LOST_MS);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("the repeat opens it, counts at 0 or not",
	    link_can_send(&pedal, LINK_MIDI, LINK_ALL), 1);
	chk("nothing written off", pedal.written_off, 0);
}

//
// A write-off is counted once, including the one that leaves tx_next at 0.
//
static void written_at_wrap(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	for (int i = 0; i < 254; i++) {
		n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
		deliver(&radio, w, n);
		link_consumed(&radio);
		ack(&radio, &pedal);
	}
	link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);	// lost
	link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);	// lost
	chk("two out, the second the last before the wrap",
	    pedal.s[0].tx_next, 0);

	tick(&pedal, pedal.now + LINK_LOST_MS);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	link_ack_again(&radio);
	ack(&radio, &pedal);
	chk("both written off, and counted once", pedal.written_off, 2);
}

//
// One packet received and held, the next lost: the held one keeps its
// place in the window however often the ack is repeated, and the lost one
// is counted once.
//
static void held_then_lost(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	n = link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);
	deliver(&radio, w, n);			// held, not consumed
	link_pack(&pedal, LINK_MIDI, LINK_ALL, LINK_FIRST, p, 1, w);	// lost

	tick(&pedal, LINK_LOST_MS);
	for (int i = 0; i < 3; i++) {
		link_ack_again(&radio);
		ack(&radio, &pedal);
	}
	chk("the held packet still in the window",
	    (uint8_t)(pedal.s[0].tx_next - pedal.s[0].tx_acked), 1);
	chk("the lost one written off once", pedal.written_off, 1);

	link_consumed(&radio);
	ack(&radio, &pedal);
	chk("window empty once the held one is consumed",
	    (uint8_t)(pedal.s[0].tx_next - pedal.s[0].tx_acked), 0);
	chk("still written off once", pedal.written_off, 1);
}

//
// A new stream takes nothing until a packet that starts a message: its
// first packet may be the rest of one from before the far side restarted,
// and numbered 0 it is in order, so only the flag can tell.
//
static void new_stream(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], tail[2] = { 0x12, 0x34 }, cc[3] = { 0xB0, 1, 2 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	n = link_pack(&pedal, LINK_MIDI, 3, 0, tail, sizeof(tail), w);
	chk("new stream: the rest of a message is dropped",
	    deliver(&radio, w, n), LINK_GOT_NOTHING);
	chk("new stream: and is not a gap", radio.gaps, 0);
	n = link_pack(&pedal, LINK_MIDI, 3, LINK_FIRST, cc, sizeof(cc), w);
	chk("new stream: a message start is taken",
	    deliver(&radio, w, n), LINK_GOT_DATA);
	link_consumed(&radio);
	ack(&radio, &pedal);
	chk("new stream: both acknowledged",
	    (uint8_t)(pedal.s[0].tx_next - pedal.s[0].tx_acked), 0);
}

//
// A side whose loop is held up reads acknowledgements that are older than
// they look: the far side sent them before this side's queued packets got
// out.  After a jump in its clock they must not write anything off; after
// the same time passing a millisecond at a time, they must.
//
static void stalled(bool jump)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], a[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n, an;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);
	deliver(&pedal, (const uint8_t[]){ 0 }, 1);

	n = link_pack(&radio, LINK_DEBUG, LINK_ALL, LINK_FIRST, p, 1, w);
	deliver(&pedal, w, n);
	link_consumed(&pedal);
	ack(&pedal, &radio);

	// Two more, still in the radio's transmit ring
	link_pack(&radio, LINK_DEBUG, LINK_ALL, LINK_FIRST, p, 1, w);
	link_pack(&radio, LINK_DEBUG, LINK_ALL, LINK_FIRST, p, 1, w);

	// The pedal says again how far it has got, which is not that far
	link_ack_again(&pedal);
	an = link_pack_ack(&pedal, a);

	if (jump)
		link_tick(&radio, radio.now + LINK_LOST_MS + 100);
	else
		tick(&radio, radio.now + LINK_LOST_MS + 100);
	deliver(&radio, a, an);
	chk(jump ? "after a stall, nothing written off"
		 : "after the same time, both written off",
	    radio.written_off, jump ? 0 : 2);
}

//
// The receiver is told when a packet follows a gap, so that the parser
// behind the link does not take it as the rest of a message cut short.
//
static void gap_flag(void)
{
	struct link pedal, radio;
	uint8_t w[LINK_WIRE_MAX], p[1] = { 0x90 };
	size_t n;

	link_init(&pedal);
	link_init(&radio);
	pedal.up = radio.up = true;
	deliver(&radio, (const uint8_t[]){ 0 }, 1);

	n = link_pack(&pedal, LINK_MIDI, 1, LINK_FIRST, p, 1, w);
	deliver(&radio, w, n);
	chk("gap flag: a new stream's first packet", radio.after_gap, 1);
	link_consumed(&radio);

	n = link_pack(&pedal, LINK_MIDI, 1, LINK_FIRST, p, 1, w);
	deliver(&radio, w, n);
	chk("gap flag: the next in order", radio.after_gap, 0);
	link_consumed(&radio);

	link_pack(&pedal, LINK_MIDI, 1, LINK_FIRST, p, 1, w);	// lost
	n = link_pack(&pedal, LINK_MIDI, 1, 0, p, 1, w);
	chk("gap flag: the rest of the lost message is dropped",
	    deliver(&radio, w, n), LINK_GOT_NOTHING);
	n = link_pack(&pedal, LINK_MIDI, 1, LINK_FIRST, p, 1, w);
	deliver(&radio, w, n);
	chk("gap flag: the next message start", radio.after_gap, 1);
}

int main(void)
{
	coding();
	damage();
	flow();
	eviction();
	loss();
	held_then_lost();
	lost_start();
	lost_wrap();
	new_stream();
	stalled(true);
	stalled(false);
	gap_flag();
	written_at_wrap();
	printf("test-link: %d packets and the flow control, %s\n", PACKETS,
	       fails ? "FAILED" : "all as expected");
	return fails != 0;
}
