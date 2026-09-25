/*
 * The MIDI service, as far as being visible goes.
 *
 * It advertises and it carries the two UUIDs a host looks for, so a
 * scanner sees a MIDI device and a connection finds the characteristic
 * where the notes will eventually be.  Nothing is sent and anything
 * written is accepted and dropped: the wire to the RP2354 is not
 * plumbed to this yet, and the point for now is that the far side of
 * the radio can be seen at all.
 *
 * Neither UUID is ours to choose - they are the ones every BLE MIDI
 * host already looks for.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define BT_UUID_MIDI_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x03b80e5a, 0xede8, 0x4b33, 0xa751, 0x6ce34ec4c700)
#define BT_UUID_MIDI_IO_VAL \
	BT_UUID_128_ENCODE(0x7772e5db, 0x3868, 0x4112, 0xa1a9, 0xf2669d106bf3)

static struct bt_uuid_128 midi_service_uuid =
	BT_UUID_INIT_128(BT_UUID_MIDI_SERVICE_VAL);
static struct bt_uuid_128 midi_io_uuid =
	BT_UUID_INIT_128(BT_UUID_MIDI_IO_VAL);

//
// A read of the characteristic returns nothing, which is what the
// specification asks for: there is no state to read, only a stream to
// be notified of.
//
static ssize_t midi_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	return 0;
}

static ssize_t midi_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	printk("midi: %u bytes in, dropped\n", len);
	return len;
}

static void midi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("midi: notifications %s\n",
	       value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(midi_svc,
	BT_GATT_PRIMARY_SERVICE(&midi_service_uuid),
	BT_GATT_CHARACTERISTIC(&midi_io_uuid.uuid,
			       BT_GATT_CHRC_READ |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP |
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       midi_read, midi_write, NULL),
	BT_GATT_CCC(midi_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

//
// Flags, the name, and the service UUID: 3 + 7 + 18 bytes of the 31 an
// advertisement has.  The UUID is in there because that is what makes a
// scanner call this a MIDI device rather than an unknown one.
//
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MIDI_SERVICE_VAL),
};

void midi_ble_start(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		printk("bt: enable failed, %d\n", err);
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      NULL, 0);
	if (err) {
		printk("bt: advertising failed, %d\n", err);
		return;
	}

	printk("bt: advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
}
