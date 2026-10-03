/*
 * LE Audio from a connected phone: the radio as a unicast server (ASCS)
 * with sink endpoints only, so a phone can play to it the way it plays to
 * earbuds.  The frames go to sink.c.
 *
 * Exploratory, after Zephyr's cap_acceptor sample.  The phone does all
 * the deciding - it picks a configuration from what sink.c published and
 * walks each endpoint through configure, QoS, enable and start - so what
 * is here is checking the configuration, splitting each packet into its
 * frames, and starting the receiver once the phone has enabled a stream.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "sink.h"
#include "midi.h"

#define STREAMS		CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT

/* Zephyr's cap_acceptor sample's preferences, which a Pixel takes */
static const struct bt_bap_qos_cfg_pref qos_pref = BT_BAP_QOS_CFG_PREF(
	true, BT_GAP_LE_PHY_2M, 2u, 20u, 20000u, 40000u, 20000u, 40000u);

/*
 * One endpoint's stream, and how its packets are laid out: 'blocks' of
 * frames in time, each of one frame per channel, in the order of the
 * channel allocation's bits.
 */
static struct ustream {
	struct bt_bap_stream s;
	bool used;
	bool up;
	uint8_t chans;			/* channels in each block */
	uint8_t ch[2];			/* ...and which of ours each is */
	uint8_t blocks;
	uint16_t octets;		/* in each frame */
} streams[STREAMS];

static struct ustream *of(struct bt_bap_stream *s)
{
	return CONTAINER_OF(s, struct ustream, s);
}

static void feeding(void)
{
	unsigned int chans = 0;

	for (int i = 0; i < STREAMS; i++) {
		if (!streams[i].up)
			continue;
		for (int c = 0; c < streams[i].chans; c++)
			chans |= 1u << streams[i].ch[c];
	}
	sink_feeding(SINK_UNICAST, chans);
	bcast_pause(chans != 0);
	midi_ble_quiet(chans != 0);
}

/*
 * The configuration the phone picked, into 'u'.  Front left and right are
 * our left and right; a stream with no allocation is mono, and is left.
 */
static int parse(struct ustream *u, const struct bt_audio_codec_cfg *cfg,
		 struct bt_bap_ascs_rsp *rsp)
{
	enum bt_audio_location loc;
	int freq, dur, octets, blocks, n = 0;

	freq = bt_audio_codec_cfg_get_freq(cfg);
	dur = bt_audio_codec_cfg_get_frame_dur(cfg);
	octets = bt_audio_codec_cfg_get_octets_per_frame(cfg);
	blocks = bt_audio_codec_cfg_get_frame_blocks_per_sdu(cfg, true);
	if (bt_audio_codec_cfg_get_chan_allocation(cfg, &loc, true))
		loc = BT_AUDIO_LOCATION_MONO_AUDIO;

	printk("unicast: %d Hz, %d us, allocation 0x%x, %d octets, "
	       "%d frames a packet\n",
	       freq > 0 ? bt_audio_codec_cfg_freq_to_freq_hz(freq) : freq,
	       dur > 0 ? bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur) : dur,
	       loc, octets, blocks);

	if (freq != BT_AUDIO_CODEC_CFG_FREQ_48KHZ ||
	    dur != BT_AUDIO_CODEC_CFG_DURATION_10 ||
	    octets <= 0 || octets > SINK_FRAME_MAX || blocks < 1) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				       BT_BAP_ASCS_REASON_CODEC_DATA);
		return -ENOTSUP;
	}

	for (uint32_t bits = loc; bits; bits &= bits - 1) {
		uint32_t bit = bits & -bits;

		if (n == ARRAY_SIZE(u->ch)) {
			*rsp = BT_BAP_ASCS_RSP(
				BT_BAP_ASCS_RSP_CODE_CONF_UNSUPPORTED,
				BT_BAP_ASCS_REASON_CODEC_DATA);
			return -ENOTSUP;
		}
		u->ch[n] = bit == BT_AUDIO_LOCATION_FRONT_RIGHT ? 1 :
			   bit == BT_AUDIO_LOCATION_FRONT_LEFT ? 0 : n;
		n++;
	}
	if (!n)
		u->ch[n++] = 0;
	u->chans = n;
	u->octets = octets;
	u->blocks = blocks;
	return 0;
}

static int config(struct bt_conn *conn, const struct bt_bap_ep *ep,
		  enum bt_audio_dir dir, const struct bt_audio_codec_cfg *cfg,
		  struct bt_bap_stream **stream,
		  struct bt_bap_qos_cfg_pref *const pref,
		  struct bt_bap_ascs_rsp *rsp)
{
	struct ustream *u = NULL;
	int err;

	for (int i = 0; i < STREAMS && !u; i++)
		if (!streams[i].used)
			u = &streams[i];
	if (dir != BT_AUDIO_DIR_SINK || !u) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM,
				       BT_BAP_ASCS_REASON_NONE);
		return -ENOMEM;
	}
	err = parse(u, cfg, rsp);
	if (err)
		return err;
	u->used = true;
	*stream = &u->s;
	*pref = qos_pref;
	return 0;
}

static int reconfig(struct bt_bap_stream *stream, enum bt_audio_dir dir,
		    const struct bt_audio_codec_cfg *cfg,
		    struct bt_bap_qos_cfg_pref *const pref,
		    struct bt_bap_ascs_rsp *rsp)
{
	int err = parse(of(stream), cfg, rsp);

	if (!err)
		*pref = qos_pref;
	return err;
}

static int qos(struct bt_bap_stream *stream, const struct bt_bap_qos_cfg *q,
	       struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: packets of %u bytes every %u us, %u retries, "
	       "%u us presentation delay\n", q->sdu, q->interval, q->rtn,
	       q->pd);
	return 0;
}

/* What the phone says a stream is for, from its metadata */
static unsigned int stream_context(const uint8_t meta[], size_t len)
{
	for (size_t i = 0; i < len && i + 1 + meta[i] <= len; i += 1 + meta[i])
		if (meta[i] >= 3 &&
		    meta[i + 1] == BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT)
			return sys_get_le16(&meta[i + 2]);
	return 0;
}

/*
 * Every step the phone takes is logged: when it stops part way, the last
 * line says where.
 */
static int enable(struct bt_bap_stream *stream, const uint8_t meta[],
		  size_t meta_len, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: enable, for 0x%x\n", stream_context(meta, meta_len));
	return 0;
}

static int metadata(struct bt_bap_stream *stream, const uint8_t meta[],
		    size_t meta_len, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: now for 0x%x\n", stream_context(meta, meta_len));
	return 0;
}

static int start(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: start\n");
	return 0;
}

static int disable(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: disable\n");
	return 0;
}

static int stop(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: stop\n");
	return 0;
}

static int release(struct bt_bap_stream *stream, struct bt_bap_ascs_rsp *rsp)
{
	printk("unicast: release\n");
	return 0;
}

static const struct bt_bap_unicast_server_cb server_cb = {
	.config = config,
	.reconfig = reconfig,
	.qos = qos,
	.enable = enable,
	.start = start,
	.metadata = metadata,
	.disable = disable,
	.stop = stop,
	.release = release,
};

/* The phone has enabled a sink: say the receiver is ready */
static void enabled(struct bt_bap_stream *stream)
{
	int err = bt_bap_stream_start(stream);

	if (err)
		printk("unicast: does not start, %d\n", err);
}

static void started(struct bt_bap_stream *stream)
{
	printk("unicast: streaming\n");
	of(stream)->up = true;
	feeding();
}

static void stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	printk("unicast: stopped, reason 0x%02x\n", reason);
	of(stream)->up = false;
	feeding();
}

static void released(struct bt_bap_stream *stream)
{
	struct ustream *u = of(stream);

	u->up = false;
	u->used = false;
	feeding();
}

/* Packets that arrived, and of those, lost or the wrong size */
static struct {
	uint32_t packets, lost, wrong;
	uint16_t wrong_len;
} st;

static void recv(struct bt_bap_stream *stream,
		 const struct bt_iso_recv_info *info, struct net_buf *buf)
{
	struct ustream *u = of(stream);
	bool valid = info->flags & BT_ISO_FLAGS_VALID;
	bool ok = valid && buf->len == u->blocks * u->chans * u->octets;
	const uint8_t *p = buf->data;

	st.packets++;
	if (!valid) {
		st.lost++;
	} else if (!ok) {
		st.wrong++;
		st.wrong_len = buf->len;
	}

	for (int b = 0; b < u->blocks; b++)
		for (int c = 0; c < u->chans; c++) {
			sink_put(SINK_UNICAST, u->ch[c], p, ok ? u->octets : 0);
			p += u->octets;
		}
}

static struct bt_bap_stream_ops stream_ops = {
	.enabled = enabled,
	.started = started,
	.stopped = stopped,
	.released = released,
	.recv = recv,
};

void unicast_poll(void)
{
	static uint32_t reported_at;
	uint32_t now = k_uptime_get_32();

	if (now - reported_at < 1000)
		return;
	reported_at = now;
	if (!st.packets)
		return;
	printk("unicast: %u packets, %u lost, %u of the wrong size (%u)\n",
	       st.packets, st.lost, st.wrong, st.wrong_len);
	st.packets = st.lost = st.wrong = 0;
}

int unicast_start(void)
{
	const struct bt_bap_unicast_server_register_param param = {
		.snk_cnt = CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT,
		.src_cnt = 0,
	};
	int err;

	err = bt_bap_unicast_server_register(&param);
	if (!err)
		err = bt_bap_unicast_server_register_cb(&server_cb);
	if (err)
		return err;
	for (int i = 0; i < STREAMS; i++)
		bt_bap_stream_cb_register(&streams[i].s, &stream_ops);
	return 0;
}
