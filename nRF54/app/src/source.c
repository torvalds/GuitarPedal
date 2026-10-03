/*
 * LE Audio out: what the pedal sends the radio over i2s (audio.c), encoded
 * with LC3 for a phone that hears the radio as a headset's microphone
 * (unicast.c).
 *
 * The pedal sends 48 kHz and a call wants less, 16 to 32 kHz; the encoder
 * takes 48 kHz in and downsamples itself.  One channel goes, the left.
 *
 * Packets go at Bluetooth's pace: two are queued when the stream starts,
 * and each one the controller has sent is room for one more.  The stack
 * only counts that; the main loop encodes and sends, the same rule as for
 * decoding.  What the pedal sends arrives at its own pace, a few tens of
 * ppm from Bluetooth's, so what is waiting is held near a target by
 * dropping or repeating a sample, as sink.c does.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/net_buf.h>
#include <lc3.h>

#include "sink.h"

#define RATE		48000
#define FRAME_US	10000
#define FRAME		(RATE / 100)		/* samples per LC3 frame */

/* A call's rates, one channel, one frame a packet */
static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |
	BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10, BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1),
	26u, SINK_FRAME_MAX, 1u, BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL);

static struct bt_pacs_cap cap = { .codec_cap = &codec_cap };

#define CONTEXTS	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | SOURCE_AVAILABLE)

/*
 * What the pedal sends, the left channel, as it arrives.  4096 samples is
 * 85 ms; TARGET of it is kept waiting, which rides out the i2s link's
 * blocks of 256 arriving against packets of 480 going two at a time - with
 * only two packets' worth, some went out as silence.
 */
#define PCM_SHIFT	12
#define PCM_SIZE	(1 << PCM_SHIFT)
#define PCM_MASK	(PCM_SIZE - 1)
#define TARGET		(3 * FRAME)

static int16_t pcm[PCM_SIZE];
static uint32_t pcm_head, pcm_tail;

NET_BUF_POOL_FIXED_DEFINE(tx_pool, 2, BT_ISO_SDU_BUF_SIZE(SINK_FRAME_MAX),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct {
	struct bt_bap_stream *stream;
	int rate, octets;
	atomic_t room;			/* packets that may be queued */
	uint16_t seq;
	bool flowing;			/* the pedal's audio has reached TARGET */
	uint32_t low;			/* the least waiting, since 'calls' */
	int calls;
	int far;			/* low points in a row a frame out */
} out;

static lc3_encoder_mem_48k_t enc_mem;
static lc3_encoder_t enc;

static struct {
	uint32_t sent, silent;		/* packets, and of those, of nothing */
	uint32_t refused;		/* the stack would not take one */
	uint32_t trimmed, padded;	/* samples dropped or repeated */
	uint32_t resettled;		/* moved to the target in one step */
	uint32_t encode_us;		/* the slowest */
} st;

/* From the main loop, as audio.c reads the i2s link */
void source_put(int32_t left, int32_t right)
{
	ARG_UNUSED(right);

	if (pcm_head - pcm_tail >= PCM_SIZE)
		pcm_tail = pcm_head - TARGET;
	pcm[pcm_head++ & PCM_MASK] = left >> 16;
}

/* From the Bluetooth stack's thread */
void source_started(struct bt_bap_stream *stream, int rate, int octets)
{
	out.rate = rate;
	out.octets = octets;
	out.seq = 0;
	out.flowing = false;
	atomic_set(&out.room, 2);
	out.stream = stream;
	printk("source: %d Hz, %d bytes a packet\n", rate, octets);
}

void source_stopped(struct bt_bap_stream *stream)
{
	if (out.stream == stream)
		out.stream = NULL;
}

void source_sent(struct bt_bap_stream *stream)
{
	if (out.stream == stream)
		atomic_inc(&out.room);
}

/*
 * The next frame of what the pedal sent, or silence while there is not
 * enough of it: the stream runs whether the pedal is sending or not.
 */
static bool next_frame(int16_t *frame)
{
	uint32_t have = pcm_head - pcm_tail;
	bool pad = false;

	/*
	 * Starting at the target, not above it: what arrives comes in blocks
	 * of 256, and a full ring is cut back to the target, so a higher
	 * threshold could be stepped over every time.
	 */
	if (!out.flowing) {
		if (have < TARGET)
			return false;
		pcm_tail = pcm_head - TARGET;
		have = TARGET;
		out.flowing = true;
		out.low = UINT32_MAX;
		out.calls = 0;
		out.far = 0;
	}
	if (have < FRAME) {
		out.flowing = false;
		return false;
	}

	/*
	 * Every ten frames, a sample either way if the low point is off; or
	 * five times in a row more than a frame off, straight back to the
	 * target in one step.  A phone answering a call moved it by 700.
	 */
	if (have < out.low)
		out.low = have;
	if (++out.calls >= 10 && out.far >= 5) {
		pcm_tail += (int32_t)(out.low - TARGET);
		st.resettled++;
		out.far = 0;
		out.low = UINT32_MAX;
		out.calls = 0;
	} else if (out.calls >= 10) {
		out.far = out.low > TARGET + FRAME ||
			  out.low < TARGET - FRAME ? out.far + 1 : 0;
		if (out.low > TARGET + FRAME / 2) {
			pcm_tail++;
			st.trimmed++;
		} else if (out.low < TARGET - FRAME / 2) {
			pad = true;
			st.padded++;
		}
		out.low = UINT32_MAX;
		out.calls = 0;
	}
	for (int i = 0; i < FRAME; i++) {
		frame[i] = pcm[pcm_tail & PCM_MASK];
		if (!(pad && i == 0))
			pcm_tail++;
	}
	return true;
}

void source_poll(void)
{
	static int enc_rate;
	static uint32_t reported_at;
	static int16_t frame[FRAME];
	struct bt_bap_stream *stream = out.stream;
	uint32_t now = k_uptime_get_32();

	while (stream && atomic_get(&out.room) > 0) {
		struct net_buf *buf;
		uint32_t t0, us;
		int err;

		if (enc_rate != out.rate) {
			enc_rate = out.rate;
			enc = lc3_setup_encoder(FRAME_US, enc_rate, RATE, &enc_mem);
		}
		if (!next_frame(frame)) {
			memset(frame, 0, sizeof(frame));
			st.silent++;
		}

		buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
		if (!buf)
			break;
		net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);

		t0 = k_cycle_get_32();
		lc3_encode(enc, LC3_PCM_FORMAT_S16, frame, 1, out.octets,
			   net_buf_add(buf, out.octets));
		us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		if (us > st.encode_us)
			st.encode_us = us;

		err = bt_bap_stream_send(stream, buf, out.seq);
		if (err) {
			net_buf_unref(buf);
			st.refused++;
			break;
		}
		out.seq++;
		atomic_dec(&out.room);
		st.sent++;
	}

	if (now - reported_at >= 1000) {
		if (stream || st.sent)
			printk("source: %u sent, %u silent, %u refused, "
			       "waiting %d, %u trimmed, %u padded, "
			       "%u resettled, encode %u us\n", st.sent,
			       st.silent, st.refused,
			       (int)(pcm_head - pcm_tail), st.trimmed,
			       st.padded, st.resettled, st.encode_us);
		memset(&st, 0, sizeof(st));
		reported_at = now;
	}
}

int source_start(void)
{
	int err;

	/*
	 * Front left, as a left earbud's microphone is.  As front centre,
	 * the natural place for one microphone, a Pixel never used it: it
	 * set a call up to the speaker side alone, over and over.
	 */
	err = bt_pacs_cap_register(BT_AUDIO_DIR_SOURCE, &cap);
	if (!err)
		err = bt_pacs_set_location(BT_AUDIO_DIR_SOURCE,
					   BT_AUDIO_LOCATION_FRONT_LEFT);
	if (!err)
		err = bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SOURCE,
						     CONTEXTS);
	if (!err || err == -EALREADY)
		err = bt_pacs_set_available_contexts(BT_AUDIO_DIR_SOURCE,
						     SOURCE_AVAILABLE);
	return err == -EALREADY ? 0 : err;
}
