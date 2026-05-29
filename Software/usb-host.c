#include "pico/stdlib.h"
#include "tusb.h"
#include "usb-sync.h"

extern void handle_midi_packet(const uint8_t packet[4]);

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

static bool sync_connected = false;
static uint8_t sync_idx = 0;

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data)
{
	(void)mount_cb_data;
	sync_idx = idx;
	sync_connected = true;
}

void tuh_midi_umount_cb(uint8_t idx)
{
	if (idx == sync_idx) {
		sync_connected = false;
	}
}

void send_midi_cc(uint8_t cc, uint8_t val)
{
	if (sync_connected) {
		uint8_t packet[4] = { 0x0B, 0xB0, cc, val };
		tuh_midi_packet_write(sync_idx, packet);
		tuh_midi_write_flush(sync_idx);
	}
}

void send_midi_pc(uint8_t pc)
{
	if (sync_connected) {
		uint8_t packet[4] = { 0x0C, 0xC0, pc, 0 };
		tuh_midi_packet_write(sync_idx, packet);
		tuh_midi_write_flush(sync_idx);
	}
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes)
{
	(void)xferred_bytes;
	if (sync_connected && idx == sync_idx) {
		uint8_t packet[4];
		while (tuh_midi_packet_read(idx, packet)) {
			handle_midi_packet(packet);
		}
	}
}
