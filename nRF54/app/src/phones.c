/*
 * LE Audio headphones: the radio on the phone's side of LE Audio, a
 * unicast client, sending headphones what the pedal sends - 48 kHz mono,
 * encoded once by source.c.
 *
 * Exploratory: another of these radios, as a sink, receives it intact,
 * and no headphones have played it yet.  Getting there is a chain:
 * connect, encrypt, find the speaker endpoints, configure one for each
 * ear, set the group's timing, enable them, open their isochronous links,
 * and wait for the headphones to say they are ready.  Each callback notes
 * what happened and leaves the next step to step_work on the system work
 * queue, as bcast.c does.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/vcp.h>

#include "phones.h"
#include "sink.h"
#include "midi.h"

#define RETRY_MS	5000
#define OCTETS		100	/* in each frame, as 48_2_1 has it */

static struct bt_bap_lc3_preset preset = BT_BAP_LC3_UNICAST_PRESET_48_2_1(
	BT_AUDIO_LOCATION_FRONT_LEFT, BT_AUDIO_CONTEXT_TYPE_MEDIA);

#define EARS		2

static struct bt_bap_stream streams[EARS];
static struct bt_audio_codec_cfg cfg[EARS];
static struct bt_bap_unicast_group *group;

static struct {
	bt_addr_le_t addr;
	bool want;
	struct bt_conn *conn;
	bool encrypted;
	bool discovering, discovered;
	struct bt_bap_ep *ep[EARS];	/* the first speaker endpoints */
	int eps;
	enum bt_audio_location loc;
	int ears;			/* streams in use */
	int configuring;		/* requests sent, one at a time */
	int configured;
	int qos_setting;
	int qos_set;
	int enabling;
	int enabled;
	int linking;
	int up;
	struct bt_vcp_vol_ctlr *vol;	/* their volume, once found */
	bool vol_asked;
	bool vol_set;			/* the volume has been set once */
	bool lost;			/* start again */
} ph;

static void step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step_work, step);

static void kick(void)
{
	k_work_reschedule(&step_work, K_NO_WAIT);
}

bool phones_owns(struct bt_conn *conn)
{
	return conn && conn == ph.conn;
}

/* ------------------------------------------------------------------ */
/* The connection                                                      */
/* ------------------------------------------------------------------ */

void phones_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("phones: no connection, 0x%02x\n", err);
		ph.lost = true;
		k_work_reschedule(&step_work, K_MSEC(RETRY_MS));
		return;
	}
	printk("phones: connected\n");
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err)
		printk("phones: no encryption, %d\n", err);
}

void phones_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("phones: disconnected, 0x%02x\n", reason);
	ph.lost = true;
	k_work_reschedule(&step_work, K_MSEC(RETRY_MS));
}

void phones_encrypted(struct bt_conn *conn, bt_security_t level, int err)
{
	if (err) {
		printk("phones: encryption failed, %d\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	printk("phones: encrypted, level %d\n", level);
	ph.encrypted = true;
	kick();
}

/* ------------------------------------------------------------------ */
/* What the headphones say                                             */
/* ------------------------------------------------------------------ */

static void location(struct bt_conn *conn, enum bt_audio_dir dir,
		     enum bt_audio_location loc)
{
	if (conn == ph.conn && dir == BT_AUDIO_DIR_SINK) {
		printk("phones: location 0x%x\n", loc);
		ph.loc = loc;
	}
}

static void available(struct bt_conn *conn, enum bt_audio_context snk,
		      enum bt_audio_context src)
{
	if (conn == ph.conn)
		printk("phones: available for 0x%x, microphone 0x%x\n",
		       snk, src);
}

static void pac_record(struct bt_conn *conn, enum bt_audio_dir dir,
		       const struct bt_audio_codec_cap *cap)
{
	int freqs = bt_audio_codec_cap_get_freq(cap);

	if (conn == ph.conn && dir == BT_AUDIO_DIR_SINK)
		printk("phones: speaker takes codec 0x%02x, rates 0x%x\n",
		       cap->id, freqs);
}

static void endpoint(struct bt_conn *conn, enum bt_audio_dir dir,
		     struct bt_bap_ep *ep)
{
	if (conn == ph.conn && dir == BT_AUDIO_DIR_SINK && ph.eps < EARS)
		ph.ep[ph.eps++] = ep;
}

static void discovered(struct bt_conn *conn, int err, enum bt_audio_dir dir)
{
	if (conn != ph.conn)
		return;
	if (err) {
		printk("phones: discovery failed, %d\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	printk("phones: %d speaker endpoints\n", ph.eps);
	ph.discovered = true;
	kick();
}

/* A refusal from the headphones, at whichever step */
static void answered(const char *what, enum bt_bap_ascs_rsp_code code,
		     enum bt_bap_ascs_reason reason)
{
	if (code == BT_BAP_ASCS_RSP_CODE_SUCCESS)
		return;
	printk("phones: %s refused, code 0x%02x reason 0x%02x\n", what, code,
	       reason);
	if (ph.conn)
		bt_conn_disconnect(ph.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void config_rsp(struct bt_bap_stream *s, enum bt_bap_ascs_rsp_code c,
		       enum bt_bap_ascs_reason r)
{
	answered("configuration", c, r);
}

static void qos_rsp(struct bt_bap_stream *s, enum bt_bap_ascs_rsp_code c,
		    enum bt_bap_ascs_reason r)
{
	answered("timing", c, r);
}

static void enable_rsp(struct bt_bap_stream *s, enum bt_bap_ascs_rsp_code c,
		       enum bt_bap_ascs_reason r)
{
	answered("enable", c, r);
}

static struct bt_bap_unicast_client_cb client_cb = {
	.location = location,
	.available_contexts = available,
	.pac_record = pac_record,
	.endpoint = endpoint,
	.discover = discovered,
	.config = config_rsp,
	.qos = qos_rsp,
	.enable = enable_rsp,
};

/* ------------------------------------------------------------------ */
/* The stream                                                          */
/* ------------------------------------------------------------------ */

static void configured(struct bt_bap_stream *s,
		       const struct bt_bap_qos_cfg_pref *pref)
{
	ph.configured++;
	kick();
}

static void qos_set(struct bt_bap_stream *s)
{
	ph.qos_set++;
	kick();
}

static void enabled(struct bt_bap_stream *s)
{
	ph.enabled++;
	kick();
}

static void started(struct bt_bap_stream *s)
{
	ph.up++;
	printk("phones: streaming, %d of %d\n", ph.up, ph.ears);
	source_started(s, 48000, OCTETS);
	midi_ble_quiet(MIDI_QUIET_PHONES, true);
	if (ph.up == ph.ears)
		kick();
}

/*
 * Their volume is the sending device's to set, and what they kept from the
 * last one may be nothing: read it, set it, and unmute them.
 */
static void vol_found(struct bt_vcp_vol_ctlr *v, int err, uint8_t vocs,
		      uint8_t aics)
{
	printk("phones: volume control %s, %d\n", err ? "not found" : "found",
	       err);
	if (err)
		return;
	ph.vol = v;
	bt_vcp_vol_ctlr_read_state(v);
}

static void vol_set(struct bt_vcp_vol_ctlr *v, int err)
{
	printk("phones: volume set, %d\n", err);
	if (!err)
		bt_vcp_vol_ctlr_read_state(v);
}

static void vol_state(struct bt_vcp_vol_ctlr *v, int err, uint8_t volume,
		      uint8_t mute)
{
	if (err)
		return;
	printk("phones: volume %u%s\n", volume, mute ? ", muted" : "");

	/* One request at a time: the volume first, then the mute */
	if (!ph.vol_set) {
		ph.vol_set = true;
		bt_vcp_vol_ctlr_set_vol(v, 200);
	} else if (mute) {
		bt_vcp_vol_ctlr_unmute(v);
	}
}

static struct bt_vcp_vol_ctlr_cb vol_cb = {
	.discover = vol_found,
	.state = vol_state,
	.vol_set = vol_set,
};

static void stopped(struct bt_bap_stream *s, uint8_t reason)
{
	printk("phones: stopped, 0x%02x\n", reason);
	source_stopped(s);
	if (ph.up && !--ph.up)
		midi_ble_quiet(MIDI_QUIET_PHONES, false);
}

static struct bt_bap_stream_ops stream_ops = {
	.configured = configured,
	.qos_set = qos_set,
	.enabled = enabled,
	.started = started,
	.stopped = stopped,
	.sent = source_sent,
};

/* ------------------------------------------------------------------ */
/* The next thing to do                                                */
/* ------------------------------------------------------------------ */

static void forget(void)
{
	struct bt_conn *conn = ph.conn;
	bt_addr_le_t addr = ph.addr;
	bool want = ph.want;

	if (group) {
		bt_bap_unicast_group_delete(group);
		group = NULL;
	}
	memset(&ph, 0, sizeof(ph));
	ph.addr = addr;
	ph.want = want;
	if (conn)
		bt_conn_unref(conn);
}

static void step(struct k_work *work)
{
	int err = 0;
	int *undo = NULL;		/* the count to take back on -EBUSY */
	const char *what = "";

	if (ph.lost)
		forget();
	if (!ph.want)
		return;

	if (!ph.conn) {
		err = bt_conn_le_create(&ph.addr, BT_CONN_LE_CREATE_CONN,
					BT_LE_CONN_PARAM_DEFAULT, &ph.conn);
		if (err) {
			printk("phones: cannot connect, %d\n", err);
			ph.conn = NULL;
			k_work_reschedule(&step_work, K_MSEC(RETRY_MS));
		}
		return;
	}

	if (ph.encrypted && !ph.discovering) {
		ph.discovering = true;
		err = bt_bap_unicast_client_discover(ph.conn, BT_AUDIO_DIR_SINK);
	} else if (ph.discovered && ph.eps && !ph.configuring) {
		/*
		 * A stream for each ear the headphones say they have, one
		 * channel each, sent the same frame.  A pair that is one
		 * device for both ears refused one stream of both channels.
		 */
		uint32_t left = ph.loc & -ph.loc;
		uint32_t rest = ph.loc & ~left;
		uint32_t at[EARS] = { left ? left : BT_AUDIO_LOCATION_FRONT_LEFT,
				      rest & -rest };

		ph.ears = at[1] && ph.eps > 1 ? 2 : 1;
		for (int i = 0; i < ph.ears; i++) {
			cfg[i] = preset.codec_cfg;
			bt_audio_codec_cfg_set_chan_allocation(&cfg[i], at[i]);
		}
		ph.configuring = 1;
		undo = &ph.configuring;
		what = "configure";
		err = bt_bap_stream_config(ph.conn, &streams[0], ph.ep[0],
					   &cfg[0]);
	} else if (ph.configured == ph.configuring &&
		   ph.configuring < ph.ears) {
		/* The control point takes one request at a time */
		int i = ph.configuring++;

		undo = &ph.configuring;
		what = "configure";
		err = bt_bap_stream_config(ph.conn, &streams[i], ph.ep[i],
					   &cfg[i]);
	} else if (ph.configured == ph.ears && !ph.qos_setting) {
		struct bt_bap_unicast_group_stream_param sp[EARS];
		struct bt_bap_unicast_group_stream_pair_param pair[EARS];
		struct bt_bap_unicast_group_param gp = {
			.params = pair,
			.params_count = ph.ears,
			.packing = BT_ISO_PACKING_SEQUENTIAL,
		};

		for (int i = 0; i < ph.ears; i++) {
			sp[i] = (struct bt_bap_unicast_group_stream_param){
				.stream = &streams[i],
				.qos = &preset.qos,
			};
			pair[i] = (struct bt_bap_unicast_group_stream_pair_param){
				.tx_param = &sp[i],
			};
		}

		ph.qos_setting = 1;
		undo = &ph.qos_setting;
		what = "timing";
		if (!group)
			err = bt_bap_unicast_group_create(&gp, &group);
		if (!err)
			err = bt_bap_stream_qos(ph.conn, group);
	} else if (ph.qos_set == ph.ears && ph.enabled == ph.enabling &&
		   ph.enabling < ph.ears) {
		int i = ph.enabling++;

		undo = &ph.enabling;
		what = "enable";
		err = bt_bap_stream_enable(&streams[i], preset.codec_cfg.meta,
					   preset.codec_cfg.meta_len);
	} else if (ph.up && ph.up == ph.ears && !ph.vol_asked) {
		struct bt_vcp_vol_ctlr *v;

		ph.vol_asked = true;
		err = bt_vcp_vol_ctlr_discover(ph.conn, &v);
		printk("phones: asking for their volume, %d\n", err);
		err = 0;
	} else if (ph.enabled == ph.ears && !ph.linking) {
		ph.linking = 1;
		undo = &ph.linking;
		what = "link";
		for (int i = 0; i < ph.ears && !err; i++) {
			err = bt_bap_stream_connect(&streams[i]);
			if (err == -EALREADY)
				err = 0;
		}
	}

	/* Still busy with the last request: the same step again shortly */
	if (err == -EBUSY && undo) {
		(*undo)--;
		k_work_reschedule(&step_work, K_MSEC(20));
		return;
	}
	if (err) {
		printk("phones: stuck at %s, %d\n", what, err);
		bt_conn_disconnect(ph.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

void phones_to(const bt_addr_le_t *addr)
{
	char s[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, s, sizeof(s));
	printk("phones: to %s\n", s);
	if (ph.conn)
		bt_conn_disconnect(ph.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	bt_addr_le_copy(&ph.addr, addr);
	ph.want = true;
	bcast_pause(BCAST_PAUSE_PHONES, true);
	kick();
}

int phones_start(void)
{
	int err = bt_bap_unicast_client_register_cb(&client_cb);

	for (int i = 0; i < EARS && !err; i++)
		bt_bap_stream_cb_register(&streams[i], &stream_ops);
	if (!err)
		err = bt_vcp_vol_ctlr_cb_register(&vol_cb);
	return err;
}
