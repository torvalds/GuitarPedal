/*
 * LE Audio in: what the radio says it can receive, the LC3 frames that
 * arrive, decoded by the main loop, and handed to the i2s link (audio.c)
 * at the link's rate.  The way out is source.c.
 *
 * Two sources feed it, a broadcast (bcast.c) and a phone's unicast stream
 * (unicast.c), and one plays at a time.  Unicast wins, because a phone
 * only starts streaming when somebody holding it asked it to.
 *
 * The frames arrive on the Bluetooth stack's thread, which only copies
 * them; the main loop decodes, the same rule as everything else on the
 * radio.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/audio/vcp.h>
#include <math.h>
#include <lc3.h>

#include "sink.h"
#include "phones.h"

#define RATE		48000
#define FRAME_US	10000
#define FRAME		(RATE / 100)		/* samples per LC3 frame */
#define CHANNELS	2

/*
 * 10 ms frames, at 48 kHz for music and down to 16 kHz for a call, and
 * one or two channels a stream with up to two frames of each in a packet:
 * a phone sends stereo as one stream of both.  Whatever the rate, the
 * decoder gives 48 kHz.
 */
static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |
	BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
	BT_AUDIO_CODEC_CAP_DURATION_10,
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1, 2), 26u, SINK_FRAME_MAX, 2u,
	BT_AUDIO_CONTEXT_TYPE_MEDIA);

static struct bt_pacs_cap cap = { .codec_cap = &codec_cap };

/*
 * Unspecified is supported, as it has to be, but not offered as available:
 * a phone takes that to mean anything at all, and offered it, a Pixel set
 * up streams for things the radio could not do and then stopped playing to
 * it.
 */
#define CONTEXTS	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | SINK_AVAILABLE)

/*
 * LC3 frames as they arrive, one channel each, 80 ms of stereo.  A frame
 * that did not arrive is kept as one with no bytes, which the decoder
 * conceals.
 */
#define Q_SHIFT		4
#define Q_SIZE		(1 << Q_SHIFT)
#define Q_MASK		(Q_SIZE - 1)

static struct frame {
	uint8_t ch;
	uint16_t len;
	uint8_t data[SINK_FRAME_MAX];
} q[Q_SIZE];
static uint16_t q_head, q_tail;
static struct k_spinlock q_lock;

/* What each source is sending, at what rate, and which is being played */
static unsigned int feeding[SINK_FROMS];
static int rate[SINK_FROMS];
static int from = -1;

static struct {
	uint32_t frames;	/* LC3 frames decoded */
	uint32_t concealed;	/* ...of which made up for a lost one */
	uint32_t dropped;	/* no room in the queue */
	uint32_t trimmed, padded;	/* samples dropped or repeated */
	uint32_t resettled;		/* moved to the target in one step */
	uint32_t underruns;
	uint32_t decode_us;	/* the slowest frame */
	uint32_t gap_ms;	/* the longest between two calls */
} st;

void sink_feeding(enum sink_from src, unsigned int chans, int hz)
{
	k_spinlock_key_t key = k_spin_lock(&q_lock);

	feeding[src] = chans;
	rate[src] = hz;
	from = feeding[SINK_UNICAST] ? SINK_UNICAST :
	       feeding[SINK_BCAST] ? SINK_BCAST : -1;
	k_spin_unlock(&q_lock, key);
}

void sink_put(enum sink_from src, int ch, const uint8_t *data, uint16_t len)
{
	k_spinlock_key_t key = k_spin_lock(&q_lock);

	if ((int)src != from) {
		/* not the one playing */
	} else if ((uint16_t)(q_head - q_tail) == Q_SIZE) {
		st.dropped++;
	} else {
		struct frame *f = &q[q_head++ & Q_MASK];

		f->ch = ch;
		f->len = len <= SINK_FRAME_MAX ? len : 0;
		memcpy(f->data, data, f->len);
	}
	k_spin_unlock(&q_lock, key);
}

/*
 * Decoded audio, a ring per channel, read in step by sink_pcm().  4096
 * samples is 85 ms; the aim is to keep TARGET of it waiting, enough to ride
 * out a frame or two arriving late.
 */
#define PCM_SHIFT	12
#define PCM_SIZE	(1 << PCM_SHIFT)
#define PCM_MASK	(PCM_SIZE - 1)
#define TARGET		(3 * FRAME)

static int16_t pcm[CHANNELS][PCM_SIZE];
static uint32_t pcm_head[CHANNELS], pcm_tail;
static bool playing;
static int played = -1;		/* the source being played */

/*
 * The least waiting over the last 100 ms.  What is waiting rises by a frame
 * at every LC3 frame and falls by a block at every i2s block, so the level
 * at any one moment says little; its low point over many of both says how
 * far ahead the decoder really is.
 */
static uint32_t low;
static int calls;
static bool settled;		/* the first low point has been acted on */
static int far;			/* low points in a row more than a frame out */

static lc3_decoder_mem_48k_t dec_mem[CHANNELS];
static lc3_decoder_t dec[CHANNELS];
static int dec_rate;

/*
 * At most two frames off the queue each time, whichever channels they
 * are for, so that a burst of them does not keep the main loop from
 * audio.c for longer than the i2s driver has queued: 21 ms, which a
 * handful of frames' decoding would fill.
 */
void sink_poll(void)
{
	static uint32_t reported_at, called_at;
	unsigned int chans;
	struct frame f;
	uint32_t now = k_uptime_get_32();

	if (called_at && now - called_at > st.gap_ms)
		st.gap_ms = now - called_at;
	called_at = now;

	/* Set up for what is being played, from the main loop */
	if (from >= 0 && rate[from] && rate[from] != dec_rate) {
		dec_rate = rate[from];
		for (int i = 0; i < CHANNELS; i++)
			dec[i] = lc3_setup_decoder(FRAME_US, dec_rate, RATE,
						   &dec_mem[i]);
		printk("sink: decoding %d Hz\n", dec_rate);
	}

	for (int n = 0; n < CHANNELS; n++) {
		k_spinlock_key_t key = k_spin_lock(&q_lock);
		bool any = q_head != q_tail;

		if (any)
			f = q[q_tail++ & Q_MASK];
		k_spin_unlock(&q_lock, key);
		if (!any)
			break;

		/* No room for a frame: the i2s side has stopped taking */
		if (pcm_head[f.ch] - pcm_tail > PCM_SIZE - FRAME)
			continue;

		/* Decode into a frame's worth of the ring, which may wrap */
		static int16_t out[FRAME];

		uint32_t t0 = k_cycle_get_32(), us;

		lc3_decode(dec[f.ch], f.len ? f.data : NULL, f.len,
			   LC3_PCM_FORMAT_S16, out, 1);
		us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		if (us > st.decode_us)
			st.decode_us = us;
		for (int i = 0; i < FRAME; i++)
			pcm[f.ch][pcm_head[f.ch]++ & PCM_MASK] = out[i];
		st.frames++;
		if (!f.len)
			st.concealed++;
	}

	/* One channel only: the other is a copy of it */
	chans = from >= 0 ? feeding[from] : 0;
	if (chans == 1 || chans == 2) {
		int a = chans == 2, b = !a;

		while (pcm_head[b] != pcm_head[a]) {
			pcm[b][pcm_head[b] & PCM_MASK] =
				pcm[a][pcm_head[b] & PCM_MASK];
			pcm_head[b]++;
		}
	}

	unicast_poll();
	source_poll();
	if (now - reported_at >= 1000) {
		if (from >= 0 || st.frames)
			printk("sink: %s, %u frames, %u concealed, "
			       "%u dropped, waiting %d, %s, %u trimmed, "
			       "%u padded, %u resettled, %u underruns, "
			       "decode %u us, "
			       "gap %u ms\n",
			       from == SINK_UNICAST ? "unicast" :
			       from == SINK_BCAST ? "broadcast" : "nothing",
			       st.frames, st.concealed, st.dropped,
			       (int)(MIN(pcm_head[0], pcm_head[1]) - pcm_tail),
			       playing ? "playing" : "not playing",
			       st.trimmed, st.padded, st.resettled,
			       st.underruns,
			       st.decode_us, st.gap_ms);
		memset(&st, 0, sizeof(st));
		reported_at = now;
	}
}

/*
 * The volume a phone sets, through the Volume Control Service, the way it
 * sets an earbud's: 0 to 255 and a mute.  The service says nothing about
 * the curve.  A Pixel sends its 25 steps as 10, 20, 31 ... 255, and this
 * takes 1 to 255 as 40 dB in equal steps - 1.6 dB a press - with 0 as
 * off: half way still has to be heard under a guitar.
 *
 * As a gain on a 16-bit sample, 65536 for none.  Set from the Bluetooth
 * stack's thread and read whole by sink_pcm().
 */
static volatile int32_t gain = 65536;

static void volume_set(struct bt_conn *conn, int err, uint8_t volume,
		       uint8_t mute)
{
	if (err)
		return;
	gain = mute || !volume ? 0 :
	       (int32_t)lrintf(65536.0f * powf(10.0f,
				(volume - 255) * (40.0f / 254) / 20.0f));
	printk("sink: volume %u%s\n", volume, mute ? ", muted" : "");
}

static struct bt_vcp_vol_rend_cb volume_cb = { .state = volume_set };

/*
 * The next 'frames' of what is being received as 32-bit stereo frames for
 * the i2s link, or false if there is nothing to play and the caller should
 * send something else.
 *
 * The sender runs on its own clock and the link on the pedal's, a few tens
 * of ppm apart, so what is waiting drifts.  Crudely for now: one sample is
 * dropped or repeated whenever it is more than half an LC3 frame from
 * where it should be.  That is an occasional click; a resampler is the
 * fix.
 */
bool sink_pcm(int32_t *out, int frames)
{
	uint32_t have = MIN(pcm_head[0], pcm_head[1]) - pcm_tail;
	bool pad;

	/* Nothing to play, or something new: start again from empty */
	if (from < 0 || from != played) {
		playing = false;
		played = from;
		pcm_head[1] = pcm_head[0] = pcm_tail;
		return false;
	}
	if (!playing) {
		if (have < TARGET + FRAME)
			return false;
		playing = true;
		settled = false;
		far = 0;
		low = UINT32_MAX;
		calls = 0;
	}
	if (have < (uint32_t)frames) {
		st.underruns++;
		playing = false;
		memset(out, 0, frames * 2 * sizeof(int32_t));
		return true;
	}

	/*
	 * Once every 100 ms or so: if the low point has been more than half
	 * a frame above the target, skip a sample; if as far below, play one
	 * twice.  Inside that is arrival jitter rather than drift.
	 */
	pad = false;
	if (have < low)
		low = have;
	if (++calls >= 19 && (!settled || far >= 5)) {
		/*
		 * The first low point, or five in a row more than a frame
		 * out: move straight to the target, back over what has just
		 * played if need be, rather than a sample at a time with a
		 * click each.  The start of a stream can arrive late enough
		 * to leave the first one far out.
		 */
		pcm_tail += (int32_t)(low - TARGET);
		if (settled)
			st.resettled++;
		settled = true;
		far = 0;
		calls = 0;
		low = UINT32_MAX;
	} else if (calls >= 19) {
		far = low > TARGET + FRAME || low < TARGET - FRAME ? far + 1 : 0;
		if (low > TARGET + FRAME / 2) {
			pcm_tail++;
			st.trimmed++;
		} else if (low < TARGET - FRAME / 2) {
			pad = true;
			st.padded++;
		}
		calls = 0;
		low = UINT32_MAX;
	}
	int32_t g = gain;

	for (int i = 0; i < frames; i++) {
		uint32_t at = pcm_tail & PCM_MASK;

		/* The low byte clear: 0xC3 there is the test pattern */
		out[2 * i] = (pcm[0][at] * g) & ~0xff;
		out[2 * i + 1] = (pcm[1][at] * g) & ~0xff;
		if (!(pad && i == 0))
			pcm_tail++;
	}
	return true;
}

void sink_start(void)
{
	const struct bt_pacs_register_param pacs = {
		.snk_pac = true,
		.snk_loc = true,
		.src_pac = true,
		.src_loc = true,
	};
	int err;

	dec_rate = RATE;
	for (int i = 0; i < CHANNELS; i++)
		dec[i] = lc3_setup_decoder(FRAME_US, RATE, RATE, &dec_mem[i]);

	err = bt_pacs_register(&pacs);
	if (!err)
		err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (!err)
		err = bt_pacs_set_location(BT_AUDIO_DIR_SINK,
					   BT_AUDIO_LOCATION_FRONT_LEFT |
					   BT_AUDIO_LOCATION_FRONT_RIGHT);
	if (!err)
		err = bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SINK,
						     CONTEXTS);
	if (!err || err == -EALREADY)
		err = bt_pacs_set_available_contexts(BT_AUDIO_DIR_SINK,
						     SINK_AVAILABLE);
	if (err && err != -EALREADY) {
		printk("sink: no capabilities, %d\n", err);
		return;
	}
	err = source_start();
	if (err)
		printk("sink: no microphone, %d\n", err);

	/*
	 * Full, until a phone says otherwise, which it does on connecting:
	 * a broadcast has nothing else to set it, and the pedal's own Level
	 * is what sets how loud it is against the guitar.
	 */
	struct bt_vcp_vol_rend_register_param vol = {
		.step = 16,
		.volume = 255,
		.cb = &volume_cb,
	};

	volume_set(NULL, 0, vol.volume, 0);
	err = bt_vcp_vol_rend_register(&vol);
	if (err)
		printk("sink: no volume control, %d\n", err);

	err = unicast_start();
	if (err)
		printk("sink: no unicast, %d\n", err);
	err = phones_start();
	if (err)
		printk("sink: no headphones, %d\n", err);
	err = bcast_start();
	if (err)
		printk("sink: no broadcast, %d\n", err);
}
