#ifndef _USB_SYNC_H
#define _USB_SYNC_H

#include <stdint.h>

// MIDI Control Change (CC) constants
#define MIDI_CC_POT_START        10 // CC 10-19 map to pots 0-9
#define MIDI_CC_GLOBAL_ENABLE    20
#define MIDI_CC_EFFECT_ENABLE    21
#define MIDI_CC_ACTIVE_POT       22

#define MIDI_CC_EFFECT_INTENSE   30
#define MIDI_CC_AUDIO_CLIPPING   31
#define MIDI_CC_CPU_LATENCY      32

// MIDI Program Change maps to effect_idx directly (0-N)

extern void send_midi_cc(uint8_t cc, uint8_t val);
extern void send_midi_pc(uint8_t pc);

extern int current_midi_effect_idx;

#endif // _USB_SYNC_H
