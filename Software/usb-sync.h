#ifndef _USB_SYNC_H
#define _USB_SYNC_H

#include <stdint.h>

enum ui_msg_type {
	MSG_STATE_UPDATE = 0,
	MSG_SYNC_REQUEST = 1,
	MSG_LED_UPDATE = 2
};

struct ui_sync_report {
	uint8_t msg_type;
	uint8_t effect_idx;
	uint8_t enabled;
	int8_t  values[10];
	uint8_t led_clipping;
	uint8_t led_intense;
	uint8_t padding[1];
} __attribute__((packed));

#endif // _USB_SYNC_H
