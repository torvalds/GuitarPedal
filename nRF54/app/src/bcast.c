/*
 * An LE Audio broadcast sink: find a broadcast, sync to it, and pass its
 * LC3 frames to sink.c.
 *
 * Exploratory, after Zephyr's bap_broadcast_sink sample with everything
 * this does not use taken out - no Broadcast Assistant, no USB, no
 * encrypted broadcasts.  Finding and syncing is a chain of callbacks, each
 * of which notes what happened and leaves the next step to step_work on
 * the system work queue.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/sys/byteorder.h>

#include "sink.h"
#include "scan.h"

#define CHANNELS	2

/* ------------------------------------------------------------------ */
/* Finding and syncing                                                 */
/* ------------------------------------------------------------------ */

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

/*
 * Not looking while a phone streams: a scan takes the radio's receiver
 * away from the phone's stream.
 */
static bool paused;

void bcast_pause(bool pause)
{
	paused = pause;
	kick();
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

	if (paused && (b.scanning || b.found || b.pa || b.sink))
		b.lost = true;
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
	if (paused)
		return;

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

static int channel_of(struct bt_bap_stream *stream)
{
	return stream == &streams[1] ? 1 : 0;
}

static void stream_recv(struct bt_bap_stream *stream,
			const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	bool ok = info->flags & BT_ISO_FLAGS_VALID;

	sink_put(SINK_BCAST, channel_of(stream), buf->data, ok ? buf->len : 0);
}

/*
 * One BIS is heard in both channels, two are left and right.  At 48 kHz,
 * taken as read: a broadcast at another rate would want its BASE read.
 */
static void streams_changed(void)
{
	sink_feeding(SINK_BCAST, !b.streams_up ? 0 :
				 b.streams_up == 1 ? 1 : 3, 48000);
}

static void stream_started(struct bt_bap_stream *stream)
{
	b.streams_up++;
	streams_changed();
}

static void stream_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	if (b.streams_up)
		b.streams_up--;
	streams_changed();
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started,
	.stopped = stream_stopped,
	.recv = stream_recv,
};

int bcast_start(void)
{
	static struct bt_bap_scan_delegator_cb delegator;
	int err;

	err = bt_bap_scan_delegator_register(&delegator);
	if (err)
		return err;
	bt_bap_broadcast_sink_register_cb(&sink_cb);
	bt_le_per_adv_sync_cb_register(&pa_cb);
	bt_le_scan_cb_register(&scan_cb);
	for (int i = 0; i < CHANNELS; i++)
		bt_bap_stream_cb_register(&streams[i], &stream_ops);
	kick();
	return 0;
}
