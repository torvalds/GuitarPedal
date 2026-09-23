#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#include "Audio/types.h"

int init_usb(void);
void usb_audio_task(void);

// What the pedal enumerates as.  Must be set before init_usb().
void usb_set_product(const char *name);

// Provide access to the output buffer
unsigned get_audio_samples(int32_t *buffer, unsigned nr);

// Read one sample from USB audio input
sample_t get_usb_audio_input(void);

//
// The host's volume and mute.  Two feature units, one at each end of the
// device: the speaker is what the host turns down to make the music
// quieter, the microphone is what it turns down to make the capture
// quieter.  usb-volume.h holds the state and the arithmetic, because the
// dB conversion wants the pow2 table pedal.c already has.
//
enum { USB_FU_SPK, USB_FU_MIC };

//
// What the descriptor tells the host it can ask for.  It stops at unity
// because this is a digital gain and above 0 dB it can only clip - the
// head of usb-volume.h has the argument.
//
#define USB_VOL_MIN_DB	(-90)
#define USB_VOL_MAX_DB	0

// What to answer the host with, or NULL if it named a channel we have not got.
int16_t *usb_volume_ptr(unsigned unit, unsigned ch);
bool *usb_mute_ptr(unsigned unit, unsigned ch);

// ...and what the host changing it does.  Both clamp and recompute.
bool usb_volume_set(unsigned unit, unsigned ch, int db_256);
bool usb_mute_set(unsigned unit, unsigned ch, bool mute);

// Apply it, in place, to interleaved stereo frames.
void usb_scale_capture(int32_t *buf, unsigned frames);
void usb_scale_playback(raw_sample_t *buf, unsigned frames);

#endif
