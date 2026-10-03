/*
 * A load for testing the link (link.h) itself, which both sides run.
 *
 * Each side sends a number of test streams at once, and checks every test
 * packet it receives: each carries its stream and a counter, and its length
 * and contents follow from those two, zero bytes included so that COBS has
 * something to do.  One stream can be made stuck: its receiver holds the
 * packets without consuming them for a while, so that stream's window stays
 * shut while everything else - the other test streams, commands, debug text
 * and real MIDI - has to keep moving.
 *
 * The pedal starts a run and asks for the radio's counts on test stream 0,
 * which is the stream used to talk about the test rather than to load it.
 */
#ifndef LINKTEST_H
#define LINKTEST_H

#include "link.h"

#define LINKTEST_STREAMS	4

/* What goes on test stream 0, by its first byte */
#define LINKTEST_START	1	/* the settings, as linktest_cfg_pack() */
#define LINKTEST_ASK	2	/* please send your counts */
#define LINKTEST_COUNTS	3	/* the counts, as linktest_counts_pack() */

struct linktest_cfg {
	uint8_t streams;	/* test streams each way, 1 to LINKTEST_STREAMS */
	uint16_t count;		/* packets on each */
	uint8_t stuck;		/* the stream whose receiver holds, or 0xff */
	uint16_t stuck_ms;	/* for how long from the start */
	uint16_t lines;		/* debug lines the radio prints meanwhile */
};

struct linktest {
	struct linktest_cfg cfg;
	bool running;
	uint32_t started;

	uint16_t sent[LINKTEST_STREAMS];	/* packets sent on each */
	uint16_t next[LINKTEST_STREAMS];	/* counter expected next */
	uint32_t got, bad, lost, early;		/* received: fine, wrong, */
						/* missing, and repeated */
	bool holding;				/* stuck packets unconsumed */
	uint8_t held_seq;
	uint32_t held;				/* how many were held */
	uint16_t lines;				/* lines still to print */
	uint8_t turn;				/* round-robin over streams */
};

static inline void linktest_start(struct linktest *t,
				  const struct linktest_cfg *cfg, uint32_t now)
{
	*t = (struct linktest){ .cfg = *cfg, .running = true, .started = now,
				.lines = cfg->lines };
	if (t->cfg.streams > LINKTEST_STREAMS)
		t->cfg.streams = LINKTEST_STREAMS;
}

/* Seven bits a byte, because the pedal takes the settings over SysEx */
static inline size_t linktest_cfg_pack(const struct linktest_cfg *c,
				       uint8_t *out)
{
	const uint8_t b[] = {
		LINKTEST_START, c->streams, c->count & 0x7f, c->count >> 7,
		c->stuck, c->stuck_ms & 0x7f, c->stuck_ms >> 7,
		c->lines & 0x7f, c->lines >> 7,
	};

	for (size_t i = 0; i < sizeof(b); i++)
		out[i] = b[i];
	return sizeof(b);
}

static inline bool linktest_cfg_unpack(const uint8_t *in, size_t len,
				       struct linktest_cfg *c)
{
	if (len < 9 || in[0] != LINKTEST_START)
		return false;
	c->streams = in[1];
	c->count = in[2] | in[3] << 7;
	c->stuck = in[4] == 0x7f ? 0xff : in[4];
	c->stuck_ms = in[5] | in[6] << 7;
	c->lines = in[7] | in[8] << 7;
	return true;
}

#define LINKTEST_COUNTS_LEN	(1 + 2 * LINKTEST_STREAMS + 4 * 5)

static inline void linktest_put32(uint8_t *out, uint32_t v)
{
	out[0] = v;
	out[1] = v >> 8;
	out[2] = v >> 16;
	out[3] = v >> 24;
}

static inline uint32_t linktest_get32(const uint8_t *in)
{
	return in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
}

static inline size_t linktest_counts_pack(const struct linktest *t,
					  uint8_t *out)
{
	size_t n = 0;

	out[n++] = LINKTEST_COUNTS;
	for (int i = 0; i < LINKTEST_STREAMS; i++) {
		out[n++] = t->sent[i];
		out[n++] = t->sent[i] >> 8;
	}
	linktest_put32(out + n, t->got), n += 4;
	linktest_put32(out + n, t->bad), n += 4;
	linktest_put32(out + n, t->lost), n += 4;
	linktest_put32(out + n, t->early), n += 4;
	linktest_put32(out + n, t->held), n += 4;
	return n;
}

static inline bool linktest_counts_unpack(const uint8_t *in, size_t len,
					  struct linktest *t)
{
	size_t n = 1;

	if (len < LINKTEST_COUNTS_LEN || in[0] != LINKTEST_COUNTS)
		return false;
	for (int i = 0; i < LINKTEST_STREAMS; i++, n += 2)
		t->sent[i] = in[n] | in[n + 1] << 8;
	t->got = linktest_get32(in + n), n += 4;
	t->bad = linktest_get32(in + n), n += 4;
	t->lost = linktest_get32(in + n), n += 4;
	t->early = linktest_get32(in + n), n += 4;
	t->held = linktest_get32(in + n);
	return true;
}

/* A test packet's payload, from its stream and counter */
static inline size_t linktest_fill(uint8_t stream, uint16_t cnt, uint8_t *out)
{
	size_t len = 3 + (cnt * 7u + stream * 13u) % (LINK_PAYLOAD_MAX - 2);

	out[0] = stream;
	out[1] = cnt;
	out[2] = cnt >> 8;
	for (size_t j = 3; j < len; j++)
		out[j] = (uint8_t)(cnt + j * 31 + stream);
	return len;
}

/*
 * The next test packet this side may send: which stream, ready in 'out',
 * or -1 if none.
 */
static inline int linktest_next(struct linktest *t, struct link *l,
				uint8_t *out, size_t *len)
{
	if (!t->running)
		return -1;
	for (int k = 0; k < t->cfg.streams; k++) {
		int i = (t->turn + k) % t->cfg.streams;

		if (t->sent[i] < t->cfg.count &&
		    link_can_send(l, LINK_TEST, i + 1)) {
			*len = linktest_fill(i, t->sent[i]++, out);
			t->turn = i + 1;
			return i;
		}
	}
	return -1;
}

/*
 * A test packet that has arrived: checked and counted.  Returns false if
 * it is to be held rather than consumed, because it is on the stuck stream
 * and the stuck time has not run out.
 */
static inline bool linktest_got(struct linktest *t, uint8_t seq,
				const uint8_t *p, size_t len, uint32_t now)
{
	uint8_t want[LINK_PAYLOAD_MAX];
	uint16_t cnt;
	uint8_t s;

	if (len < 3 || p[0] >= LINKTEST_STREAMS) {
		t->bad++;
		return true;
	}
	s = p[0];
	cnt = p[1] | p[2] << 8;
	if (linktest_fill(s, cnt, want) != len) {
		t->bad++;
	} else {
		for (size_t j = 0; j < len; j++)
			if (want[j] != p[j]) {
				t->bad++;
				goto counted;
			}
		if (cnt > t->next[s])
			t->lost += cnt - t->next[s];
		else if (cnt < t->next[s])
			t->early++;
		t->got++;
	}
counted:
	if (cnt >= t->next[s])
		t->next[s] = cnt + 1;

	if (s == t->cfg.stuck && now - t->started < t->cfg.stuck_ms) {
		t->holding = true;
		t->held_seq = seq;
		t->held++;
		return false;
	}
	// Consuming this one acknowledges whatever was held before it
	if (s == t->cfg.stuck)
		t->holding = false;
	return true;
}

/* Consume what the stuck stream held, once its time is up */
static inline void linktest_release(struct linktest *t, struct link *l,
				    uint32_t now)
{
	if (t->holding && now - t->started >= t->cfg.stuck_ms) {
		link_done(l, LINK_TEST, t->cfg.stuck + 1, t->held_seq);
		t->holding = false;
	}
}

#endif /* LINKTEST_H */
