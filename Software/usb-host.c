#include "pico/stdlib.h"
#include "tusb.h"
#include "usb-sync.h"

extern void handle_ui_sync_report(const struct ui_sync_report *sync);

// Needed because audio/effect.h expects it
float get_usb_audio_input(void)
{
	return 0.0f;
}

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

int init_usb(void)
{
	const tusb_rhport_init_t rh_init = {
		.role = TUSB_ROLE_HOST,
		.speed = TUSB_SPEED_FULL
	};
	return tusb_init(BOARD_TUH_RHPORT, &rh_init);
}

static uint8_t sync_dev_addr = 0;
static uint8_t sync_instance = 0;

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *desc_report, uint16_t desc_len)
{
	(void)desc_report;
	(void)desc_len;
	uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

	if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
		sync_dev_addr = dev_addr;
		sync_instance = instance;
		tuh_hid_receive_report(dev_addr, instance);
	}
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
	if (dev_addr == sync_dev_addr && instance == sync_instance) {
		sync_dev_addr = 0;
	}
}

void send_ui_sync_report(const struct ui_sync_report *rep)
{
	if (sync_dev_addr) {
		tuh_hid_send_report(sync_dev_addr, sync_instance, 0, rep, sizeof(struct ui_sync_report));
	}
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len)
{
	uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

	if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
		if (len >= sizeof(struct ui_sync_report)) {
			const struct ui_sync_report *sync = (const struct ui_sync_report *)report;
			handle_ui_sync_report(sync);
		}
	}

	tuh_hid_receive_report(dev_addr, instance);
}
