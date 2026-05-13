#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>

int init_usb(void);
void usb_audio_task(void);

// Provide access to the output buffer
unsigned get_audio_samples(int32_t *buffer, unsigned nr);

#endif
