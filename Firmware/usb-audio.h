#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>

#include "Audio/types.h"

int init_usb(void);
void usb_audio_task(void);

// What the pedal enumerates as.  Must be set before init_usb().
void usb_set_product(const char *name);

// Provide access to the output buffer
unsigned get_audio_samples(int32_t *buffer, unsigned nr);

// Read one sample from USB audio input
sample_t get_usb_audio_input(void);

#endif
