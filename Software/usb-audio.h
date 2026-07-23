#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>

#include "audio/types.h"

int init_usb(void);
void usb_audio_task(void);

// Provide access to the output buffer
unsigned get_audio_samples(int32_t *buffer, unsigned nr);

// Read one sample from USB audio input
sample_t get_usb_audio_input(void);

#endif
