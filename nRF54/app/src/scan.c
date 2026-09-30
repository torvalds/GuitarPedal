/*
 * Listing what is advertising nearby, so a person can pick their
 * controller out of it by name.
 *
 * Nothing here works out what a device is.  An advertisement does not
 * say - the foot controller this was written for advertises Human
 * Interface Device and keeps the MIDI service to itself - and settling
 * it means connecting to every stranger in range to read its attribute
 * table.  A player knows what their own footswitch is called.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "scan.h"
#include "midi.h"

#ifdef CONFIG_BT_OBSERVER

//
// Long enough for the slow ones.
//
// Six seconds found a controller that was advertising briskly and missed
// devices that were not: an advertising interval is the peer's business
// and some of them are seconds apart.  Thirty is long enough to be worth
// pressing once, and it can be stopped as soon as what you wanted shows
// up - which is why the button says stop while it runs.
//
#define SCAN_SECONDS		30
#define SCAN_MAX_SEEN		16

/*
 * What the scan saw.
 *
 * The address is what a later connection is made to; the name is the
 * only part a person can recognise, and it usually arrives in the scan
 * response rather than in the advertisement.
 */
static struct seen {
	bt_addr_le_t addr;
	char name[SCAN_NAME_MAX];
} seen[SCAN_MAX_SEEN];

static unsigned int nr_seen;
static bool running;

static struct seen *lookup(const bt_addr_le_t *addr)
{
	for (unsigned int i = 0; i < nr_seen; i++)
		if (!bt_addr_le_cmp(&seen[i].addr, addr))
			return &seen[i];

	return NULL;
}

static struct seen *remember(const bt_addr_le_t *addr)
{
	struct seen *s = lookup(addr);

	if (s)
		return s;

	if (nr_seen == SCAN_MAX_SEEN)
		return NULL;

	bt_addr_le_copy(&seen[nr_seen].addr, addr);
	return &seen[nr_seen++];
}

static bool ad_name(struct bt_data *data, void *user_data)
{
	struct seen *s = user_data;

	if (data->type != BT_DATA_NAME_COMPLETE &&
	    data->type != BT_DATA_NAME_SHORTENED)
		return true;

	size_t len = MIN(data->data_len, sizeof(s->name) - 1);

	memcpy(s->name, data->data, len);
	s->name[len] = '\0';
	return false;		/* the name is all this wanted */
}

static void device_seen(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			struct net_buf_simple *ad)
{
	struct seen *s;

	switch (type) {
	/* Only what can be connected to; a beacon cannot be bound. */
	case BT_GAP_ADV_TYPE_ADV_IND:
	case BT_GAP_ADV_TYPE_ADV_DIRECT_IND:
		s = remember(addr);
		break;

	/*
	 * The answer to the scan request, and usually where the name
	 * is: the foot controller's advertisement has no room for one
	 * beside the service it claims.  It says nothing about whether
	 * the device is worth listing, so it names one already seen
	 * rather than introducing a new one.
	 */
	case BT_GAP_ADV_TYPE_SCAN_RSP:
		s = lookup(addr);
		break;

	default:
		return;
	}

	if (!s)
		return;

	if (!s->name[0]) {
		struct net_buf_simple copy = *ad;

		bt_data_parse(&copy, ad_name, s);
	}
}

static void looking_done(struct k_work *work)
{
	ARG_UNUSED(work);

	bt_le_scan_stop();

	for (unsigned int i = 0; i < nr_seen; i++)
		scan_found(&seen[i].addr, seen[i].name);

	running = false;
	scan_done(nr_seen);
}
static K_WORK_DELAYABLE_DEFINE(look_work, looking_done);

//
// Stop now and report what has been seen so far.
//
// The same ending as running out of time, because there is only one:
// whoever asked has what they wanted or has given up, and either way the
// list is what it is.  Reusing looking_done() rather than repeating it is
// what keeps the two from drifting.
//
void scan_stop(void)
{
	if (!running)
		return;

	k_work_cancel_delayable(&look_work);
	looking_done(NULL);
}

void scan_start(void)
{
	int err;

	if (running)
		return;

	nr_seen = 0;
	memset(seen, 0, sizeof(seen));
	running = true;

	/* Active, because the name is in the scan response. */
	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_seen);
	if (err) {
		running = false;
		scan_done(0);
		return;
	}

	k_work_schedule(&look_work, K_SECONDS(SCAN_SECONDS));
}

#endif /* CONFIG_BT_OBSERVER */
