/*
 * The link between the RP2354 and the radio, as packets.
 *
 * Shared by both sides: the radio includes it as "link.h", the pedal as
 * "nRF54/app/src/link.h", so the two cannot disagree about the format.
 *
 * A packet is a header, up to LINK_PAYLOAD_MAX bytes and a CRC-8 of both,
 * COBS-encoded and ended by a zero byte.  COBS (Consistent Overhead Byte
 * Stuffing) removes every zero from what it encodes, so a zero on the wire
 * is always the end of a packet: either side can start listening at any
 * point, or reset in the middle of one, and the other loses at most the
 * packet that was cut.  The CRC catches a byte that is wrong rather than
 * missing, which COBS alone mostly decodes into a packet that looks fine.
 *
 * The header is four bytes:
 *
 *	kind	what the packet carries: MIDI, a command, debug text, an
 *		acknowledgement, or the radio's hello
 *	peer	which Bluetooth peer it is from or for; 0 is all of them.
 *		The radio hands these out, and the pedal only ever echoes
 *		one back, so it keeps no table of them.
 *	seq	the packet's number within its stream
 *	flags	LINK_FIRST, LINK_LAST, LINK_TRUSTED
 *
 * A stream is a kind and a peer.  Each stream is flow-controlled on its own:
 * no more than LINK_WINDOW packets may be sent beyond the last one the other
 * side has acknowledged, and an acknowledgement says how far the receiver
 * has got rather than how many more it can take, so one that is lost costs
 * nothing.  A big message goes as many packets, other streams' packets go
 * between them, and a stream that is stalled holds up only itself.
 *
 * Every stream starts with its window open, and nothing is granted: each
 * side's buffers have room for every stream's window at once.  What has to
 * hold is that the receiver is listening before the first packet.  The
 * pedal is up first, because it holds the radio in reset until it is; and
 * the radio sends a hello once it is listening, which the pedal waits for.
 * The hello is repeated until the pedal answers, in case one is lost, and
 * carries a boot id so that a repeat is not taken for a new start.  At a
 * new start the pedal forgets every stream, so a radio that resets on its
 * own starts every stream again from the beginning.
 *
 * An acknowledgement says how far the receiver has consumed, which is what
 * frees the window, and how far it has received.  They are repeated now and
 * then, since they only restate those: a lost one would otherwise leave a
 * full window shut for good.  And a sender that has sent beyond what the
 * receiver has received, and sent nothing for LINK_LOST_MS, takes the rest
 * as lost on the wire and stops counting it against the window; otherwise,
 * with every packet in the window lost, nothing newer would ever arrive to
 * be acknowledged.  The receiver sees the next packet as a gap.  A receiver
 * that is merely slow has received everything sent, so its window stays
 * shut until it catches up.
 *
 * A receiver that has heard nothing on a stream has nothing to repeat, so a
 * sender with a packet unacknowledged for LINK_LOST_MS asks for an ack.  The
 * ask carries how far the sender has been acknowledged, which a receiver
 * with no record of the stream takes as how far it has got: the packets it
 * never saw are then written off like any others.
 */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kinds */
#define LINK_MIDI	0	/* MIDI bytes, to or from the air */
#define LINK_CONTROL	1	/* a command for the radio, or its answer */
#define LINK_DEBUG	2	/* text from the radio, for a person */
#define LINK_ACK	3	/* seq is how far the receiver has got */
#define LINK_HELLO	4	/* the radio has just started */
#define LINK_ASK	5	/* a packet has gone unacknowledged: ack, please */
#define LINK_TEST	6	/* load for testing the link itself */

/* Peers */
#define LINK_ALL	0

/* Flags */
#define LINK_FIRST	0x01	/* the payload starts a message */
#define LINK_LAST	0x02	/* the payload ends a message */
#define LINK_TRUSTED	0x04	/* from a peer bonded with Secure Connections */

#define LINK_HEADER		4
#define LINK_PAYLOAD_MAX	128
#define LINK_FRAME_MAX		(LINK_HEADER + LINK_PAYLOAD_MAX + 1)
#define LINK_WIRE_MAX		(LINK_FRAME_MAX + 2)

/* Packets in flight per stream, and streams either side keeps track of */
#define LINK_WINDOW	2
#define LINK_STREAMS	12

/* How long a packet the receiver has not seen may be on its way */
#define LINK_LOST_MS	200

/*
 * CRC-8 with polynomial 0x07, as SMBus uses: it catches every error of one
 * bit, of an odd number of bits, and of a burst up to eight bits long.
 */
static const uint8_t link_crc8_table[256] = {
	0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
	0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
	0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65,
	0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
	0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5,
	0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
	0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85,
	0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
	0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2,
	0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
	0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2,
	0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
	0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32,
	0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
	0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42,
	0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
	0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c,
	0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
	0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec,
	0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
	0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c,
	0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
	0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c,
	0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
	0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b,
	0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
	0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b,
	0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
	0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb,
	0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
	0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb,
	0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
};

static inline uint8_t link_crc8(uint8_t crc, uint8_t b)
{
	return link_crc8_table[crc ^ b];
}

/*
 * Encode a packet into 'out', which must hold LINK_WIRE_MAX bytes, and
 * return how many bytes it took, the terminating zero included.  A packet
 * is shorter than COBS's 254-byte block, so it costs exactly one byte of
 * overhead and the zero.
 */
static inline size_t link_encode(uint8_t kind, uint8_t peer, uint8_t seq,
				 uint8_t flags, const uint8_t *payload,
				 size_t len, uint8_t *out)
{
	const uint8_t header[LINK_HEADER] = { kind, peer, seq, flags };
	size_t code_at = 0, o = 1;
	uint8_t code = 1, crc = 0;

	for (size_t i = 0; i <= LINK_HEADER + len; i++) {
		uint8_t b;

		if (i < LINK_HEADER)
			b = header[i];
		else if (i < LINK_HEADER + len)
			b = payload[i - LINK_HEADER];
		else
			b = crc;
		crc = link_crc8(crc, b);

		if (b) {
			out[o++] = b;
			code++;
		}
		if (!b || code == 0xff) {
			out[code_at] = code;
			code_at = o++;
			code = 1;
		}
	}
	out[code_at] = code;
	out[o++] = 0;
	return o;
}

/*
 * A packet being received, a byte at a time.
 *
 * Nothing is believed until the first zero has been seen, because a
 * receiver that starts mid-packet would otherwise decode the tail of one as
 * a whole one.
 */
struct link_rx {
	uint8_t buf[LINK_FRAME_MAX];
	uint16_t len;
	uint8_t left;		/* bytes still to come in this COBS block */
	uint8_t code;		/* the block's code, 0xff for "no zero after it" */
	bool synced;
	bool done;		/* buf holds a packet already handed over */
	bool over;		/* longer than a packet can be: drop it */
	uint32_t bad;		/* packets dropped as malformed */
	uint32_t crc;		/* packets dropped for a wrong CRC */

	/*
	 * The packet as it came off the wire, for showing one that was
	 * dropped: 'failed' is set by the zero that ended it, and lasts
	 * until the next byte.
	 */
	uint8_t wire[LINK_WIRE_MAX];
	uint16_t wire_len;
	bool ended;		/* the last byte was a zero */
	uint8_t failed;		/* 0, or LINK_FAILED_BAD or _CRC */
};

#define LINK_FAILED_BAD	1
#define LINK_FAILED_CRC	2

static inline void link_rx_init(struct link_rx *rx)
{
	rx->len = 0;
	rx->left = 0;
	rx->code = 0xff;
	rx->synced = false;
	rx->done = false;
	rx->over = false;
	rx->bad = 0;
	rx->crc = 0;
	rx->wire_len = 0;
	rx->ended = false;
	rx->failed = 0;
}

static inline bool link_rx_crc_ok(const struct link_rx *rx)
{
	uint8_t crc = 0;

	for (uint16_t i = 0; i + 1 < rx->len; i++)
		crc = link_crc8(crc, rx->buf[i]);
	return crc == rx->buf[rx->len - 1];
}

/*
 * Take one byte off the wire.  Returns true when it completes a packet,
 * which is then in rx->buf, header first, rx->len bytes in all, the CRC
 * already checked and taken off.  It stays there until the next byte.
 */
static inline bool link_rx_byte(struct link_rx *rx, uint8_t b)
{
	if (rx->ended) {
		rx->wire_len = 0;
		rx->ended = false;
		rx->failed = 0;
	}
	if (b && rx->wire_len < sizeof(rx->wire))
		rx->wire[rx->wire_len++] = b;

	if (!b) {
		bool whole;

		if (rx->done)		/* that one has been handed over */
			rx->len = 0;
		whole = rx->synced && !rx->over && rx->left == 0 &&
			rx->len > LINK_HEADER;

		if (rx->synced && !whole && (rx->len || rx->over)) {
			rx->bad++;
			rx->failed = LINK_FAILED_BAD;
		} else if (whole && !link_rx_crc_ok(rx)) {
			rx->crc++;
			rx->failed = LINK_FAILED_CRC;
			whole = false;
		}
		if (whole)
			rx->len--;
		rx->ended = true;
		rx->synced = true;
		rx->done = whole;
		rx->over = false;
		rx->left = 0;
		rx->code = 0xff;
		if (!whole)
			rx->len = 0;
		return whole;
	}

	if (!rx->synced || rx->over)
		return false;

	/* A packet just handed over is replaced by the next one */
	if (rx->done) {
		rx->done = false;
		rx->len = 0;
	}

	if (rx->left == 0) {
		if (rx->code != 0xff) {
			if (rx->len == sizeof(rx->buf)) {
				rx->over = true;
				return false;
			}
			rx->buf[rx->len++] = 0;
		}
		rx->code = b;
		rx->left = b - 1;
		return false;
	}

	if (rx->len == sizeof(rx->buf)) {
		rx->over = true;
		return false;
	}
	rx->buf[rx->len++] = b;
	rx->left--;
	return false;
}

/*
 * Both directions of one stream, as one side sees them.
 */
struct link_stream {
	bool used;
	uint8_t kind, peer;
	uint8_t tx_next;	/* the seq the next packet out gets */
	uint8_t tx_acked;	/* the first one the far side has not consumed */
	uint8_t rx_next;	/* the seq expected next */
	uint8_t rx_done;	/* one past the last consumed: what an ack says */
	bool rx_skip;		/* a packet went missing: wait for LINK_FIRST */
	bool rx_seen;		/* anything has been received on it */
	bool ack_due;
	uint32_t touched;	/* when it last carried anything, for eviction */
	uint32_t sent_at;	/* when the last packet went out */
	uint8_t written_to;	/* tx_next when a loss was last counted */
	bool written;		/* and whether one has been */
	uint32_t asked_at;	/* when an ack was last asked for */
};

struct link {
	struct link_stream s[LINK_STREAMS];
	struct link_rx rx;
	bool after_gap;		/* packets before the one in rx were lost */
	bool up;		/* a hello has been seen */
	uint32_t boot;		/* the id in the last hello */
	uint32_t gaps;		/* packets that never arrived */
	uint32_t refused;	/* packets with nowhere to go */
	uint32_t written_off;	/* packets sent and taken as lost */
	uint32_t clock;		/* counts packets, to say which stream is oldest */
	uint32_t now;		/* in milliseconds, as link_tick() last said */
};

static inline void link_forget(struct link *l)
{
	for (int i = 0; i < LINK_STREAMS; i++)
		l->s[i].used = false;
}

static inline void link_init(struct link *l)
{
	link_forget(l);
	link_rx_init(&l->rx);
	l->up = false;
	l->gaps = l->refused = l->written_off = 0;
	l->clock = 0;
	l->now = 0;
}

/*
 * The stream for a kind and peer, made if 'make' and there is room.
 *
 * A free slot if there is one.  Otherwise the stream that has gone longest
 * without carrying anything, provided it has nothing in flight and nothing
 * to acknowledge; if the far side still remembers it, the sequence numbers
 * will disagree, which is counted as a gap and recovered from like any
 * other.  A new stream takes nothing until a packet that starts a message,
 * since its first packet may be the rest of one from a stream the far side
 * had before.  Peers come and go, each with streams of their own, so taking the
 * first idle slot instead would evict a stream in steady use.
 */
static inline struct link_stream *link_stream(struct link *l, uint8_t kind,
					      uint8_t peer, bool make)
{
	struct link_stream *idle = NULL, *s;

	for (int i = 0; i < LINK_STREAMS; i++) {
		s = &l->s[i];

		if (s->used && s->kind == kind && s->peer == peer) {
			s->touched = ++l->clock;
			return s;
		}
	}
	if (!make)
		return NULL;

	for (int i = 0; i < LINK_STREAMS; i++) {
		s = &l->s[i];

		if (!s->used) {
			idle = s;
			break;
		}
		if (s->tx_next == s->tx_acked && !s->ack_due &&
		    (!idle || (int32_t)(s->touched - idle->touched) < 0))
			idle = s;
	}
	if (!idle)
		return NULL;

	idle->used = true;
	idle->kind = kind;
	idle->peer = peer;
	idle->tx_next = idle->tx_acked = 0;
	idle->rx_next = idle->rx_done = 0;
	idle->rx_skip = true;
	idle->rx_seen = false;
	idle->written = false;
	idle->ack_due = false;
	idle->touched = ++l->clock;
	idle->asked_at = l->now;
	return idle;
}

/*
 * The time, for telling a lost packet from one still on its way.
 *
 * A jump means this side's loop was held up.  What it had queued mostly
 * could not leave meanwhile, and an acknowledgement it reads now may be
 * from before its packets did, so the time does not count towards a
 * packet being lost.
 */
static inline void link_tick(struct link *l, uint32_t now_ms)
{
	uint32_t gap = now_ms - l->now;

	if (gap > LINK_LOST_MS / 2) {
		for (int i = 0; i < LINK_STREAMS; i++) {
			l->s[i].sent_at += gap;
			l->s[i].asked_at += gap;
		}
	}
	l->now = now_ms;
}

/* May a packet go out on this stream now? */
static inline bool link_can_send(struct link *l, uint8_t kind, uint8_t peer)
{
	struct link_stream *s = link_stream(l, kind, peer, true);

	return s && (uint8_t)(s->tx_next - s->tx_acked) < LINK_WINDOW;
}

/*
 * Encode the next packet of a stream, which the caller has asked
 * link_can_send() about.
 */
static inline size_t link_pack(struct link *l, uint8_t kind, uint8_t peer,
			       uint8_t flags, const uint8_t *payload,
			       size_t len, uint8_t *out)
{
	struct link_stream *s = link_stream(l, kind, peer, true);

	s->sent_at = l->now;
	return link_encode(kind, peer, s->tx_next++, flags, payload, len, out);
}

/*
 * An acknowledgement that is due, if there is one: how far consumed in the
 * seq byte, how far received in the payload.  The acknowledged stream's
 * kind rides in the flags byte, since an ack is not part of a stream itself.
 *
 * Failing that, an ask, for a stream with a packet that has gone
 * unacknowledged for LINK_LOST_MS: the stream's kind in the flags byte
 * again, and how far it has been acknowledged in the payload.
 */
static inline size_t link_pack_ack(struct link *l, uint8_t *out)
{
	for (int i = 0; i < LINK_STREAMS; i++) {
		struct link_stream *s = &l->s[i];

		if (s->used && s->ack_due) {
			s->ack_due = false;
			return link_encode(LINK_ACK, s->peer, s->rx_done,
					   s->kind, &s->rx_next, 1, out);
		}
	}
	for (int i = 0; i < LINK_STREAMS; i++) {
		struct link_stream *s = &l->s[i];

		if (s->used && s->tx_next != s->tx_acked &&
		    l->now - s->sent_at >= LINK_LOST_MS &&
		    l->now - s->asked_at >= LINK_LOST_MS) {
			s->asked_at = l->now;
			return link_encode(LINK_ASK, s->peer, 0, s->kind,
					   &s->tx_acked, 1, out);
		}
	}
	return 0;
}

/*
 * A packet has been dealt with, by its stream and number - for a receiver
 * that keeps packets for a while before it can use them.
 */
static inline void link_done(struct link *l, uint8_t kind, uint8_t peer,
			     uint8_t seq)
{
	struct link_stream *s = link_stream(l, kind, peer, false);

	if (s) {
		s->rx_done = (uint8_t)(seq + 1);
		s->ack_due = true;
	}
}

/*
 * Say again how far every stream has got.  Called now and then, so that a
 * lost acknowledgement is made good by the next one.
 */
static inline void link_ack_again(struct link *l)
{
	for (int i = 0; i < LINK_STREAMS; i++)
		if (l->s[i].used && l->s[i].rx_seen)
			l->s[i].ack_due = true;
}

/* The packet in l->rx has been dealt with */
static inline void link_consumed(struct link *l)
{
	link_done(l, l->rx.buf[0], l->rx.buf[1], l->rx.buf[2]);
}

/*
 * What a packet that has just arrived means.  LINK_GOT_DATA leaves it in
 * l->rx for the caller, payload after the header, until the caller says
 * link_consumed(); everything else the link has dealt with itself.
 *
 * l->after_gap says that packets on its stream were lost or skipped before
 * it, so that whatever parses the payload starts afresh rather than taking
 * it as the rest of a message that was cut short.  A new stream's first
 * packet counts, since it may follow a stream the far side had before.
 */
enum { LINK_GOT_NOTHING, LINK_GOT_DATA, LINK_GOT_HELLO };

static inline int link_take(struct link *l)
{
	const uint8_t *h = l->rx.buf;
	struct link_stream *s;

	switch (h[0]) {
	case LINK_HELLO: {
		uint32_t boot = 0;

		if (l->rx.len >= LINK_HEADER + 4)
			boot = h[4] | h[5] << 8 | h[6] << 16 |
			       (uint32_t)h[7] << 24;
		if (l->up && boot == l->boot)
			return LINK_GOT_NOTHING;	/* a repeat */
		link_forget(l);
		l->up = true;
		l->boot = boot;
		return LINK_GOT_HELLO;
	}

	case LINK_ACK:
		s = link_stream(l, h[3], h[1], false);
		if (!s)
			return LINK_GOT_NOTHING;
		/* Only an ack for something actually sent counts */
		if ((uint8_t)(h[2] - s->tx_acked) <=
		    (uint8_t)(s->tx_next - s->tx_acked))
			s->tx_acked = h[2];
		/*
		 * Received short of what was sent, long enough after the
		 * last send that nothing can still be on the way: the rest
		 * was lost, and only what was received and not yet
		 * consumed still holds the window.  Worked out from the
		 * ack itself, so that the same ack again changes nothing.
		 */
		if (l->rx.len > LINK_HEADER) {
			uint8_t got = h[LINK_HEADER];
			uint8_t upto = s->tx_next - (uint8_t)(got - h[2]);

			if (got != s->tx_next &&
			    (uint8_t)(got - h[2]) <=
			    (uint8_t)(s->tx_next - h[2]) &&
			    (uint8_t)(upto - s->tx_acked) <=
			    (uint8_t)(s->tx_next - s->tx_acked) &&
			    l->now - s->sent_at >= LINK_LOST_MS) {
				if (!s->written || s->written_to != s->tx_next) {
					l->written_off +=
						(uint8_t)(s->tx_next - got);
					s->written_to = s->tx_next;
					s->written = true;
				}
				s->tx_acked = upto;
			}
		}
		return LINK_GOT_NOTHING;

	case LINK_ASK:
		s = link_stream(l, h[3], h[1], false);
		/* Never heard of: everything up to the ask was lost */
		if (!s && l->rx.len > LINK_HEADER) {
			s = link_stream(l, h[3], h[1], true);
			if (s) {
				s->rx_next = s->rx_done = h[LINK_HEADER];
				s->rx_seen = true;
			}
		}
		if (s)
			s->ack_due = true;
		return LINK_GOT_NOTHING;
	}

	s = link_stream(l, h[0], h[1], true);
	if (!s) {
		l->refused++;
		return LINK_GOT_NOTHING;
	}

	if (h[2] != s->rx_next) {
		l->gaps++;
		s->rx_skip = true;
	}
	s->rx_seen = true;
	s->rx_next = (uint8_t)(h[2] + 1);

	if (s->rx_skip && !(h[3] & LINK_FIRST)) {
		link_consumed(l);
		return LINK_GOT_NOTHING;
	}
	l->after_gap = s->rx_skip;
	s->rx_skip = false;
	return LINK_GOT_DATA;
}

#endif /* LINK_H */
