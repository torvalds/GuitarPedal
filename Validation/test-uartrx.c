//
// Check where the radio's UART receive ring has got to,
// nRF54/app/src/uartrx.h, against a model of the hardware.
//
// The model is what the radio relies on: the DMA refills one ring for
// ever, END is a flag set at every wrap, and AMOUNT is the position in the
// current pass after each zero (the match filter) and the whole ring after
// END.  A zero on the ring's last byte sets AMOUNT a byte before END, the
// worst order it could happen in.  The DMA moves on between the radio's
// register reads and while it reads the ring.
//
// What is checked:
//
//  - head never runs past what the DMA has written, and never falls behind
//    a zero written before the poll began;
//  - no byte the loop reads has been overwritten, however far behind the
//    loop falls, because the overrun check skips first - and the check has
//    to fire for that to mean anything;
//  - all of that across 32-bit wrap of the positions;
//  - after a stall long enough to lose a wrap, the position is a whole
//    number of rings off and stays so, so it is the same place in the ring.
//
// Build and run with 'make test-uartrx && ./test-uartrx'.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nRF54/app/src/uartrx.h"

#define W	LINK_WIRE_MAX

static int fails;
static void chk(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL %-44s got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static uint32_t seed = 1;
static uint32_t rnd(uint32_t n)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return n ? seed % n : 0;
}

//
// The hardware.  'truth' is every byte sent, by position, far enough back
// to cover anything still in the ring.
//
#define TRUTH_SHIFT	20
#define TRUTH_MASK	((1u << TRUTH_SHIFT) - 1)

static uint8_t ring[UARTRX_SIZE];
static uint8_t truth[1u << TRUTH_SHIFT];
static uint32_t dma;			// bytes written
static uint32_t last_zero;		// position after the last zero
static uint32_t frame_left;		// bytes before the next zero
static bool hw_end, end_due;
static uint32_t hw_amount;
static uint32_t last_byte_zeros;	// zero on the ring's last byte

static void advance(uint32_t n)
{
	while (n--) {
		uint8_t b;

		if (end_due) {
			hw_end = true;
			end_due = false;
		}
		if (!frame_left) {
			b = 0;
			frame_left = 1 + rnd(W - 1);
		} else {
			b = 1 + rnd(255);
			frame_left--;
		}
		ring[dma & UARTRX_MASK] = b;
		truth[dma & TRUTH_MASK] = b;
		dma++;
		if (!b) {
			last_zero = dma;
			hw_amount = ((dma - 1) & UARTRX_MASK) + 1;
		}
		if (!(dma & UARTRX_MASK)) {
			hw_amount = UARTRX_SIZE;
			if (b) {
				hw_end = true;
			} else {
				last_byte_zeros++;
				end_due = true;
			}
		}
	}
}

// main.c's rx_written(), with the DMA moving between the reads if 'race'
static uint32_t retries;
static bool race = true;
static void poll(struct uartrx *r)
{
	bool end;
	uint32_t amount;

	for (;;) {
		end = hw_end;
		advance(race ? rnd(4) : 0);
		amount = hw_amount;
		advance(race ? rnd(4) : 0);
		if (hw_end == end)
			break;
		retries++;
	}
	if (end)
		hw_end = false;
	uartrx_written(r, end, amount);
}

static void start(struct uartrx *r, uint32_t at)
{
	memset(ring, 0, sizeof(ring));
	dma = last_zero = at;
	frame_left = 0;
	hw_end = end_due = false;
	hw_amount = 0;
	*r = (struct uartrx){ .wraps = at >> UARTRX_SHIFT, .head = at,
			      .tail = at };
}

//
// The loop falling behind by any amount: a gap between polls short of a
// ring, so that no wrap is lost, and a budget for how much it reads, which
// is sometimes far less than arrived.  Returns how many overruns it saw.
//
static uint32_t behind(const char *name, uint32_t at, int passes)
{
	struct uartrx r;
	uint32_t overruns = 0, read = 0, bad = 0, ahead = 0, missed = 0;

	start(&r, at);
	for (int i = 0; i < passes; i++) {
		uint32_t before, budget;

		advance(rnd(UARTRX_SIZE - 3 * W));
		before = last_zero;
		poll(&r);
		if ((int32_t)(r.head - dma) > 0)
			ahead++;
		if ((int32_t)(before - r.head) > 0 && !end_due)
			missed++;
		if (uartrx_overrun(&r))
			overruns++;

		// The DMA goes on while the loop reads, up to a packet
		advance(rnd(W));
		budget = rnd(4) ? UINT32_MAX : rnd(2 * W);
		while (r.tail != r.head && budget--) {
			if (ring[r.tail & UARTRX_MASK] !=
			    truth[r.tail & TRUTH_MASK])
				bad++;
			r.tail++;
			read++;
		}
	}

	char what[64];

	snprintf(what, sizeof(what), "%s: head past the DMA", name);
	chk(what, ahead, 0);
	snprintf(what, sizeof(what), "%s: head short of a zero", name);
	chk(what, missed, 0);
	snprintf(what, sizeof(what), "%s: overwritten bytes read", name);
	chk(what, bad, 0);
	printf("test-uartrx: %s, %u bytes read, %u overruns caught, "
	       "%u bytes skipped\n", name, read, overruns, r.lost);
	return overruns;
}

//
// Where head should be with nothing moving during the poll: after the last
// zero, or at the last wrap if no zero has been written since - unless that
// zero was the ring's last byte and its END has not come yet, when head
// stays where it was.
//
static uint32_t expected(const struct uartrx *r)
{
	uint32_t wrap = dma & ~(uint32_t)UARTRX_MASK;

	if (end_due)
		return r->head;
	return (int32_t)(last_zero - wrap) > 0 ? last_zero : wrap;
}

//
// A stall of more than a ring, which loses a wrap.  The position is then a
// whole number of rings short of the truth, and stays the same number of
// rings short from then on.
//
static void stall(const char *name, uint32_t at)
{
	struct uartrx r;
	uint32_t moved = 0, odd = 0;
	int32_t off = 0;
	bool have = false;

	race = false;
	start(&r, at);
	for (int i = 0; i < 200; i++) {
		uint32_t want;

		advance(rnd(UARTRX_SIZE / 2));
		want = expected(&r);
		poll(&r);
		chk("before the stall, head where expected", r.head, want);
		r.tail = r.head;
	}
	// Across two wraps, so that END is set twice and read once
	advance(2 * UARTRX_SIZE - (dma & UARTRX_MASK) + rnd(UARTRX_SIZE / 2));
	for (int i = 0; i < 2000; i++) {
		uint32_t want;

		advance(rnd(UARTRX_SIZE / 2));
		want = expected(&r);
		poll(&r);
		uartrx_overrun(&r);
		r.tail = r.head;

		// Once head has caught up with packets written after the stall
		if (i >= 2 && !end_due) {
			int32_t d = (int32_t)(want - r.head);

			if (d & UARTRX_MASK)
				odd++;
			if (have && d != off)
				moved++;
			off = d;
			have = true;
		}
	}

	char what[64];

	snprintf(what, sizeof(what), "%s: off by part of a ring", name);
	chk(what, odd, 0);
	snprintf(what, sizeof(what), "%s: offset changed", name);
	chk(what, moved, 0);
	snprintf(what, sizeof(what), "%s: a ring short", name);
	chk(what, off, UARTRX_SIZE);
	printf("test-uartrx: %s, %d rings short afterwards\n", name,
	       off / UARTRX_SIZE);
	race = true;
}

//
// A poll that lands between a match on the ring's last byte and its END:
// AMOUNT already says the whole ring, END is not set, and head must stay
// put rather than go back to the start of the pass.
//
static void last_byte(void)
{
	struct uartrx r;
	uint32_t was;

	race = false;
	start(&r, 0);
	advance(UARTRX_SIZE - W);
	poll(&r);
	frame_left = 0;
	advance(1);			// a zero
	frame_left = W - 2;
	advance(W - 1);			// a packet whose zero is the last byte
	chk("last byte: set up as meant", end_due && hw_amount == UARTRX_SIZE,
	    1);
	was = r.head;
	poll(&r);
	chk("last byte: head before END", r.head, was);
	advance(1);
	poll(&r);
	chk("last byte: head after END", r.head, UARTRX_SIZE);
	race = true;
}

int main(void)
{
	uint32_t overruns = 0;

	overruns += behind("from zero", 0, 200000);
	overruns += behind("across 32 bits", -64 * UARTRX_SIZE, 200000);
	chk("overruns happened, so the check was tested", overruns > 0, 1);
	chk("a zero landed on the ring's last byte", last_byte_zeros > 0, 1);
	chk("END changed between reads", retries > 0, 1);

	last_byte();
	stall("stall", 0);
	stall("stall across 32 bits", -2 * UARTRX_SIZE);

	if (fails)
		return 1;
	printf("test-uartrx: %u END races retried, %u zeros on the last "
	       "byte, all as expected\n", retries, last_byte_zeros);
	return 0;
}
