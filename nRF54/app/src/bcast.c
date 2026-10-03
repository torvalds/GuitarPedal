/*
 * An LE Audio broadcast sink: find a broadcast, sync to it, decode its LC3
 * and hand the audio to the i2s link (audio.c).
 *
 * Exploratory, after Zephyr's bap_broadcast_sink sample with everything
 * this does not use taken out - no Broadcast Assistant, no USB, no
 * encrypted broadcasts.  Finding and syncing is a chain of callbacks, each
 * of which notes what happened and leaves the next step to step_work on
 * the system work queue.  The audio arrives on the Bluetooth stack's
 * thread, which only copies it; the main loop decodes, the same rule as
 * everything else on the radio.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/sys/byteorder.h>
#include <lc3.h>

#include "bcast.h"
#include "scan.h"

#define RATE		48000
#define FRAME_US	10000
#define FRAME		(RATE / 100)		/* samples per LC3 frame */
#define SDU_MAX		155			/* the largest 48 kHz frame */
#define CHANNELS	2

/* ------------------------------------------------------------------ */
/* Finding and syncing                                                 */
/* ------------------------------------------------------------------ */

static const struct bt_audio_codec_cap codec_cap = BT_AUDIO_CODEC_CAP_LC3(
	BT_AUDIO_CODEC_CAP_FREQ_48KHZ, BT_AUDIO_CODEC_CAP_DURATION_10,
	BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 40u, SDU_MAX, 1u,
	BT_AUDIO_CONTEXT_TYPE_MEDIA);

static struct bt_pacs_cap cap = { .codec_cap = &codec_cap };

static struct bt_bap_stream streams[CHANNELS];
static struct bt_bap_stream *stream_ptrs[CHANNELS] = { &streams[0],
							&streams[1] };

static struct {
	bool scanning;
	bool found;		/* a broadcast to sync to */
	bt_addr_le_t addr;
	uint8_t sid;
	uint16_t interval;
	uint32_t id;
	struct bt_le_per_adv_sync *pa;
	bool pa_synced;
	struct bt_bap_broadcast_sink *sink;
	uint32_t bis;		/* the BIS indexes to sync to */
	bool syncable;
	bool syncing;
	bool lost;		/* something ended: start again */
	int streams_up;
} b;

static void step(struct k_work *work);
static K_WORK_DEFINE(step_work, step);

static void kick(void)
{
	k_work_submit(&step_work);
}

static bool announcement(struct bt_data *data, void *user_data)
{
	const struct bt_le_scan_recv_info *info = user_data;
	struct bt_uuid_16 uuid;

	if (data->type != BT_DATA_SVC_DATA16 ||
	    data->data_len < BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE)
		return true;
	if (!bt_uuid_create(&uuid.uuid, data->data, BT_UUID_SIZE_16) ||
	    bt_uuid_cmp(&uuid.uuid, BT_UUID_BROADCAST_AUDIO))
		return true;

	if (b.scanning && !b.found) {
		bt_addr_le_copy(&b.addr, info->addr);
		b.sid = info->sid;
		b.interval = info->interval;
		b.id = sys_get_le24(data->data + BT_UUID_SIZE_16);
		b.found = true;
		kick();
	}
	return false;
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *ad)
{
	/* A periodic advertiser is the only kind that can be a broadcast */
	if (info->interval && b.scanning && !b.found)
		bt_data_parse(ad, announcement, (void *)info);
}

static struct bt_le_scan_cb scan_cb = { .recv = scan_recv };

static void pa_synced(struct bt_le_per_adv_sync *sync,
		      struct bt_le_per_adv_sync_synced_info *info)
{
	if (sync == b.pa) {
		b.pa_synced = true;
		kick();
	}
}

static void pa_term(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info)
{
	if (sync == b.pa) {
		printk("bcast: periodic sync lost, reason 0x%02x\n",
		       info->reason);
		b.pa = NULL;
		b.lost = true;
		kick();
	}
}

static struct bt_le_per_adv_sync_cb pa_cb = {
	.synced = pa_synced,
	.term = pa_term,
};

static void base_recv(struct bt_bap_broadcast_sink *sink,
		      const struct bt_bap_base *base, size_t size)
{
	uint32_t bis = 0;

	if (b.bis || bt_bap_base_get_bis_indexes(base, &bis) || !bis)
		return;

	/* The first two, which a stereo broadcast has as left and right */
	b.bis = bis & -bis;
	bis &= ~b.bis;
	b.bis |= bis & -bis;
	kick();
}

static void syncable(struct bt_bap_broadcast_sink *sink,
		     const struct bt_iso_biginfo *biginfo)
{
	/* How much of each interval the broadcast keeps the radio busy */
	printk("bcast: %u BIS, %u subevents each, burst %u, repeats %u, "
	       "PDU %u, interval %u us, PHY %u\n", biginfo->num_bis,
	       biginfo->sub_evt_count, biginfo->burst_number,
	       biginfo->rep_count, biginfo->max_pdu,
	       biginfo->iso_interval * 1250, biginfo->phy);
	if (biginfo->encryption) {
		printk("bcast: encrypted, which this does not do\n");
		return;
	}
	b.syncable = true;
	kick();
}

static void sink_started(struct bt_bap_broadcast_sink *sink)
{
	printk("bcast: synced to broadcast 0x%06x\n", b.id);
}

static void sink_stopped(struct bt_bap_broadcast_sink *sink, uint8_t reason)
{
	printk("bcast: stopped, reason 0x%02x\n", reason);
	b.lost = true;
	kick();
}

static struct bt_bap_broadcast_sink_cb sink_cb = {
	.base_recv = base_recv,
	.syncable = syncable,
	.started = sink_started,
	.stopped = sink_stopped,
};

static uint16_t sync_timeout(uint16_t interval)
{
	uint32_t timeout;

	if (interval == BT_BAP_PA_INTERVAL_UNKNOWN)
		return BT_GAP_PER_ADV_MAX_TIMEOUT;
	timeout = BT_GAP_US_TO_PER_ADV_SYNC_TIMEOUT(
			BT_GAP_PER_ADV_INTERVAL_TO_US(interval)) * 5;
	return CLAMP(timeout, BT_GAP_PER_ADV_MIN_TIMEOUT,
		     BT_GAP_PER_ADV_MAX_TIMEOUT);
}

/*
 * The next thing to do, from whatever the callbacks have said.  On the
 * system work queue, where the Bluetooth calls may wait.
 */
static void step(struct k_work *work)
{
	int err;

	if (b.lost) {
		if (b.sink) {
			bt_bap_broadcast_sink_stop(b.sink);
			bt_bap_broadcast_sink_delete(b.sink);
		}
		if (b.pa)
			bt_le_per_adv_sync_delete(b.pa);
		if (b.scanning)
			scan_release();
		memset(&b, 0, sizeof(b));
	}

	if (!b.scanning && !b.found) {
		err = scan_hold();
		if (err) {
			printk("bcast: cannot scan, %d\n", err);
			return;
		}
		b.scanning = true;
		printk("bcast: looking for a broadcast\n");
		return;
	}

	if (b.found && !b.pa) {
		struct bt_le_per_adv_sync_param p = {
			.sid = b.sid,
			.skip = 5,
			.timeout = sync_timeout(b.interval),
		};

		scan_release();
		b.scanning = false;
		bt_addr_le_copy(&p.addr, &b.addr);
		printk("bcast: found broadcast 0x%06x, syncing\n", b.id);
		err = bt_le_per_adv_sync_create(&p, &b.pa);
		if (err) {
			printk("bcast: periodic sync failed, %d\n", err);
			b.lost = true;
			kick();
		}
		return;
	}

	if (b.pa_synced && !b.sink) {
		err = bt_bap_broadcast_sink_create(b.pa, b.id, &b.sink);
		if (err) {
			printk("bcast: sink not created, %d\n", err);
			b.lost = true;
			kick();
		}
		return;
	}

	if (b.sink && b.bis && b.syncable && !b.syncing) {
		b.syncing = true;
		err = bt_bap_broadcast_sink_sync(b.sink, b.bis, stream_ptrs,
						 NULL);
		if (err) {
			printk("bcast: BIG sync failed, %d\n", err);
			b.lost = true;
			kick();
		}
	}
}

/*
 * Each connection's timing, logged because connections and a synced
 * broadcast compete for the radio.
 */
static void conn_timing(struct bt_conn *conn, const char *what)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE)
		return;
	printk("bcast: connection %s, interval %u us, latency %u, "
	       "timeout %u ms, %s\n", what, info.le.interval_us,
	       info.le.latency, info.le.timeout * 10,
	       info.role == BT_CONN_ROLE_PERIPHERAL ? "peripheral" : "central");
}

static void timing_connected(struct bt_conn *conn, uint8_t err)
{
	if (!err)
		conn_timing(conn, "made");
}

static void timing_updated(struct bt_conn *conn, uint16_t interval,
			   uint16_t latency, uint16_t timeout)
{
	conn_timing(conn, "updated");
}

BT_CONN_CB_DEFINE(bcast_conn_cb) = {
	.connected = timing_connected,
	.le_param_updated = timing_updated,
};

/* ------------------------------------------------------------------ */
/* Audio in                                                            */
/* ------------------------------------------------------------------ */

/*
 * LC3 frames as they arrive, copied by the stack's thread for the main
 * loop to decode.  A frame that did not arrive is kept as one with no
 * bytes, which the decoder conceals.
 */
#define SDU_Q_SHIFT	4
#define SDU_Q_SIZE	(1 << SDU_Q_SHIFT)
#define SDU_Q_MASK	(SDU_Q_SIZE - 1)

static struct sdu {
	uint8_t ch;
	uint16_t len;
	uint8_t data[SDU_MAX];
} sdu_q[SDU_Q_SIZE];
static uint16_t sdu_head, sdu_tail;
static struct k_spinlock sdu_lock;

static struct {
	uint32_t frames;	/* LC3 frames decoded */
	uint32_t concealed;	/* ...of which made up for a lost one */
	uint32_t dropped;	/* no room in the queue */
	uint32_t trimmed, padded;	/* samples dropped or repeated */
	uint32_t underruns;
} st;

static int channel_of(struct bt_bap_stream *stream)
{
	return stream == &streams[1] ? 1 : 0;
}

static void stream_recv(struct bt_bap_stream *stream,
			const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	k_spinlock_key_t key = k_spin_lock(&sdu_lock);

	if ((uint16_t)(sdu_head - sdu_tail) == SDU_Q_SIZE) {
		st.dropped++;
	} else {
		struct sdu *s = &sdu_q[sdu_head++ & SDU_Q_MASK];

		s->ch = channel_of(stream);
		s->len = 0;
		if ((info->flags & BT_ISO_FLAGS_VALID) && buf->len <= SDU_MAX) {
			s->len = buf->len;
			memcpy(s->data, buf->data, buf->len);
		}
	}
	k_spin_unlock(&sdu_lock, key);
}

static void stream_started(struct bt_bap_stream *stream)
{
	b.streams_up++;
}

static void stream_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	if (b.streams_up)
		b.streams_up--;
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started,
	.stopped = stream_stopped,
	.recv = stream_recv,
};

/*
 * Decoded audio, a ring per channel, read in step by bcast_pcm().  4096
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

/*
 * The least waiting over the last 100 ms.  What is waiting rises by a frame
 * at every LC3 frame and falls by a block at every i2s block, so the level
 * at any one moment says little; its low point over many of both says how
 * far ahead the decoder really is.
 */
static uint32_t low;
static int calls;

static lc3_decoder_mem_48k_t dec_mem[CHANNELS];
static lc3_decoder_t dec[CHANNELS];

void bcast_poll(void)
{
	static uint32_t reported_at;
	struct sdu s;
	uint32_t now;

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&sdu_lock);
		bool any = sdu_head != sdu_tail;

		if (any)
			s = sdu_q[sdu_tail++ & SDU_Q_MASK];
		k_spin_unlock(&sdu_lock, key);
		if (!any)
			break;

		/* No room for a frame: the i2s side has stopped taking */
		if (pcm_head[s.ch] - pcm_tail > PCM_SIZE - FRAME)
			continue;

		/* Decode into a frame's worth of the ring, which may wrap */
		static int16_t out[FRAME];

		lc3_decode(dec[s.ch], s.len ? s.data : NULL, s.len,
			   LC3_PCM_FORMAT_S16, out, 1);
		for (int i = 0; i < FRAME; i++)
			pcm[s.ch][pcm_head[s.ch]++ & PCM_MASK] = out[i];
		st.frames++;
		if (!s.len)
			st.concealed++;
	}

	/* One channel only: the other is a copy of it */
	if (b.streams_up == 1 && !(b.bis & (b.bis - 1))) {
		while (pcm_head[1] != pcm_head[0]) {
			pcm[1][pcm_head[1] & PCM_MASK] =
				pcm[0][pcm_head[1] & PCM_MASK];
			pcm_head[1]++;
		}
	}

	now = k_uptime_get_32();
	if (now - reported_at >= 1000) {
		if (b.streams_up || st.frames)
			printk("bcast: %u frames, %u concealed, %u dropped, "
			       "waiting %d, %s, %u trimmed, %u padded, "
			       "%u underruns\n", st.frames, st.concealed,
			       st.dropped,
			       (int)(MIN(pcm_head[0], pcm_head[1]) - pcm_tail),
			       playing ? "playing" : "not playing",
			       st.trimmed, st.padded, st.underruns);
		memset(&st, 0, sizeof(st));
		reported_at = now;
	}
}

/*
 * The next 'frames' of the broadcast as 32-bit stereo frames for the i2s
 * link, or false if there is nothing to play and the caller should send
 * something else.
 *
 * The broadcast runs on its sender's clock and the link on the pedal's, a
 * few tens of ppm apart, so what is waiting drifts.  Crudely for now: one
 * sample is dropped or repeated whenever it strays from where it should
 * be.  That is an occasional click; a resampler is the fix.
 */
bool bcast_pcm(int32_t *out, int frames)
{
	uint32_t have = MIN(pcm_head[0], pcm_head[1]) - pcm_tail;
	bool pad;

	/* Nothing to play: start again from empty when something is */
	if (!b.streams_up) {
		playing = false;
		pcm_head[1] = pcm_head[0] = pcm_tail;
		return false;
	}
	if (!playing) {
		if (have < TARGET + FRAME)
			return false;
		playing = true;
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
	 * Once every 100 ms or so: if the low point has been well above the
	 * target, skip a frame; if well below, play one twice.
	 */
	pad = false;
	if (have < low)
		low = have;
	if (++calls >= 19) {
		if (low > TARGET) {
			pcm_tail++;
			st.trimmed++;
		} else if (low < TARGET - FRAME) {
			pad = true;
			st.padded++;
		}
		calls = 0;
		low = UINT32_MAX;
	}
	for (int i = 0; i < frames; i++) {
		uint32_t at = pcm_tail & PCM_MASK;

		out[2 * i] = (int32_t)pcm[0][at] << 16;
		out[2 * i + 1] = (int32_t)pcm[1][at] << 16;
		if (!(pad && i == 0))
			pcm_tail++;
	}
	return true;
}

void bcast_start(void)
{
	const struct bt_pacs_register_param pacs = {
		.snk_pac = true,
		.snk_loc = true,
	};
	static struct bt_bap_scan_delegator_cb delegator;
	int err;

	err = bt_pacs_register(&pacs);
	if (!err)
		err = bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &cap);
	if (!err)
		err = bt_bap_scan_delegator_register(&delegator);
	if (err) {
		printk("bcast: does not start, %d\n", err);
		return;
	}
	bt_bap_broadcast_sink_register_cb(&sink_cb);
	bt_le_per_adv_sync_cb_register(&pa_cb);
	bt_le_scan_cb_register(&scan_cb);
	for (int i = 0; i < CHANNELS; i++) {
		bt_bap_stream_cb_register(&streams[i], &stream_ops);
		dec[i] = lc3_setup_decoder(FRAME_US, RATE, RATE, &dec_mem[i]);
	}
	kick();
}
