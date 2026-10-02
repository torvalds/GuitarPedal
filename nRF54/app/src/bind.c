/*
 * The connection to a controller the player picked out of a scan.
 *
 * A footswitch is a BLE MIDI peripheral, so reaching one means being a
 * central: connect, find the MIDI characteristic, subscribe, and hand
 * what arrives to the same decoder a write from the web app goes
 * through.  It ends at midi_uart_send() either way, so the pedal cannot
 * tell the two apart and does not need to.
 *
 * One at a time, and it keeps trying.  A footswitch sleeps, goes out of
 * range and comes back, and nobody is going to open the app to say so.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "bind.h"
#include "midi.h"

#ifdef CONFIG_BT_CENTRAL

/*
 * How long to wait for pairing before reading the table anyway.
 *
 * A controller that wants an encrypted link hangs up during discovery
 * if nothing asks for one, so the request goes out first.  A controller
 * that wants nothing of the sort never answers, and the SMP timeout is
 * thirty seconds, which is not a wait to hand a player.
 */
#define ENCRYPT_WAIT_MS		2500

/* A controller that is off is the ordinary case, not a failure. */
#define RETRY_MS		5000
#define CONNECT_TIMEOUT_MS	4000

static struct bt_uuid_128 midi_service =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x03b80e5a, 0xede8, 0x4b33,
					    0xa751, 0x6ce34ec4c700));
static struct bt_uuid_128 midi_char =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7772e5db, 0x3868, 0x4112,
					    0xa1a9, 0xf2669d106bf3));

static bt_addr_le_t target;
static bool have_target;

static struct bt_conn *conn;
static struct bt_gatt_discover_params discover;
static struct bt_gatt_subscribe_params subscribe;
static uint16_t service_end;
static uint16_t write_handle;		/* the MIDI characteristic's value */

/*
 * How far the link to the bound device has got, and why it last failed:
 * a negative errno for a connection or a subscription Zephyr refused,
 * 1000 plus the reason encryption failed, 2000 plus a disconnect's
 * reason, and 3000 plus the reason a connection was not made.
 */
static uint8_t state;
static int last_err;

/*
 * The connection is dropped on the Bluetooth stack's thread and written to
 * from the main loop, so it is let go of under a lock and used under a
 * reference.
 */
static struct k_spinlock bind_lock;

static void bind_drop(void)
{
	k_spinlock_key_t key = k_spin_lock(&bind_lock);
	struct bt_conn *old = conn;

	conn = NULL;
	write_handle = 0;
	k_spin_unlock(&bind_lock, key);

	if (old)
		bt_conn_unref(old);
}

uint8_t bind_state(void)
{
	return state;
}

int bind_last_err(void)
{
	return last_err;
}
static bool discovering;

static void try_connect(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(retry_work, try_connect);

static void discover_service(void);

static void impatient(struct k_work *work)
{
	ARG_UNUSED(work);

	if (conn)
		discover_service();
}
static K_WORK_DELAYABLE_DEFINE(wait_work, impatient);

/* ------------------------------------------------------------------ */
/* What arrives once we are subscribed                                 */
/* ------------------------------------------------------------------ */

static uint8_t notified(struct bt_conn *c, struct bt_gatt_subscribe_params *p,
			const void *data, uint16_t len)
{
	if (!data) {
		/* The peer dropped the subscription. */
		p->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}

	midi_ble_controller(data, len);
	return BT_GATT_ITER_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Finding the characteristic to subscribe to                          */
/* ------------------------------------------------------------------ */

static uint8_t found_ccc(struct bt_conn *c, const struct bt_gatt_attr *attr,
			 struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		printk("bind: no CCC, cannot subscribe\n");
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	subscribe.ccc_handle = attr->handle;
	subscribe.value = BT_GATT_CCC_NOTIFY;
	subscribe.notify = notified;

	err = bt_gatt_subscribe(c, &subscribe);
	if (err) {
		last_err = err;
		printk("bind: subscribe refused\n");
	} else {
		state |= BIND_SUBSCRIBED;
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t found_char(struct bt_conn *c, const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("bind: no MIDI characteristic\n");
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	subscribe.value_handle = bt_gatt_attr_value_handle(attr);
	write_handle = subscribe.value_handle;
	state |= BIND_FOUND;

	discover.uuid = BT_UUID_GATT_CCC;
	discover.func = found_ccc;
	discover.start_handle = subscribe.value_handle + 1;
	discover.end_handle = service_end;
	discover.type = BT_GATT_DISCOVER_DESCRIPTOR;

	if (bt_gatt_discover(c, &discover))
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

	return BT_GATT_ITER_STOP;
}

static uint8_t found_service(struct bt_conn *c, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	const struct bt_gatt_service_val *val;

	if (!attr) {
		printk("bind: no MIDI service on this device\n");
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return BT_GATT_ITER_STOP;
	}

	val = attr->user_data;
	service_end = val->end_handle;

	discover.uuid = &midi_char.uuid;
	discover.func = found_char;
	discover.start_handle = attr->handle + 1;
	discover.end_handle = service_end;
	discover.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	if (bt_gatt_discover(c, &discover))
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

	return BT_GATT_ITER_STOP;
}

static void discover_service(void)
{
	if (discovering)
		return;
	discovering = true;

	discover.uuid = &midi_service.uuid;
	discover.func = found_service;
	discover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover.type = BT_GATT_DISCOVER_PRIMARY;

	if (bt_gatt_discover(conn, &discover))
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/* ------------------------------------------------------------------ */
/* Getting there, and staying there                                    */
/* ------------------------------------------------------------------ */

static void try_connect(struct k_work *work)
{
	struct bt_conn_le_create_param create =
		BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE,
					     BT_GAP_SCAN_FAST_INTERVAL,
					     BT_GAP_SCAN_FAST_WINDOW);
	int err;

	ARG_UNUSED(work);

	if (!have_target || conn)
		return;

	create.timeout = CONNECT_TIMEOUT_MS / 10;

	err = bt_conn_le_create(&target, &create, BT_LE_CONN_PARAM_DEFAULT,
				&conn);
	if (err) {
		last_err = err;
		k_work_schedule(&retry_work, K_MSEC(RETRY_MS));
	}
}

void bind_to(const bt_addr_le_t *addr)
{
	bt_addr_le_copy(&target, addr);
	have_target = true;

	if (conn) {
		/* Drop whatever we were on; the retry picks up the new one. */
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	k_work_schedule(&retry_work, K_NO_WAIT);
}

bool bind_owns(struct bt_conn *c)
{
	return c && c == conn;
}

void bind_connected(struct bt_conn *c, uint8_t err)
{
	if (err) {
		last_err = 3000 + err;
		bind_drop();
		k_work_schedule(&retry_work, K_MSEC(RETRY_MS));
		return;
	}

	printk("bind: connected\n");
	state = BIND_CONNECTED;
	discovering = false;

	/*
	 * Ask for encryption and read the table a moment later whether or
	 * not it settled - see ENCRYPT_WAIT_MS.
	 */
	if (bt_conn_set_security(c, BT_SECURITY_L2))
		discover_service();
	else
		k_work_schedule(&wait_work, K_MSEC(ENCRYPT_WAIT_MS));
}

void bind_encrypted(struct bt_conn *c, bt_security_t level, int err)
{
	k_work_cancel_delayable(&wait_work);

	if (err)
		last_err = 1000 + err;
	else
		state |= BIND_ENCRYPTED;

	//
	// A key the other end has forgotten.  Pairing is kept in RAM
	// only, so a controller that has been reset - or a radio that
	// has been reflashed - leaves the two disagreeing about a bond,
	// and every connection after that fails to encrypt with "PIN or
	// key missing" instead of pairing again.
	//
	// Throw ours away and hang up.  The next attempt has nothing to
	// remember and pairs from the start.
	//
	if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
		printk("bind: stale key, forgetting it\n");
		bt_unpair(BT_ID_DEFAULT, &target);
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	discover_service();
}

void bind_disconnected(struct bt_conn *c, uint8_t reason)
{
	printk("bind: disconnected, %u\n", reason);

	k_work_cancel_delayable(&wait_work);
	bind_drop();
	subscribe.value_handle = 0;
	state = 0;
	last_err = 2000 + reason;
	discovering = false;

	k_work_schedule(&retry_work, K_MSEC(RETRY_MS));
}

/*
 * MIDI to the bound device, as one BLE MIDI packet: a header byte, and a
 * timestamp before every status byte as RP-052 asks.  Written without a
 * response, which is what a MIDI characteristic offers.
 */
int bind_send(const uint8_t *midi, size_t len)
{
	uint8_t pkt[1 + 2 * BIND_SEND_MAX];
	uint16_t now = k_uptime_get_32() & 0x1FFF;
	struct bt_conn *c = NULL;
	uint16_t handle = 0;
	k_spinlock_key_t key;
	size_t n = 0;
	int err;

	if (len > BIND_SEND_MAX)
		return -EMSGSIZE;

	key = k_spin_lock(&bind_lock);
	if (conn && write_handle) {
		c = bt_conn_ref(conn);
		handle = write_handle;
	}
	k_spin_unlock(&bind_lock, key);
	if (!c)
		return -ENOTCONN;

	pkt[n++] = 0x80 | (now >> 7);
	for (size_t i = 0; i < len; i++) {
		if (midi[i] & 0x80)
			pkt[n++] = 0x80 | (now & 0x7F);
		pkt[n++] = midi[i];
	}

	err = bt_gatt_write_without_response(c, handle, pkt, n, false);
	bt_conn_unref(c);
	return err;
}

#endif /* CONFIG_BT_CENTRAL */
