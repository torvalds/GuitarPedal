#ifndef _AUDIO_TYPES_H
#define _AUDIO_TYPES_H

#include "pico/platform/sections.h"

//
// Mark a function as running on cpu1, the hard-realtime audio core.
//
// Two things fall out of this.  The linker script pulls anything in
// '.time_critical*' out of flash and into RAM, so these don't take XIP
// cache misses.  And the 'audio_' prefix on the section name is what
// scripts/check-audio.py keys off when it checks that nothing in here
// calls out to newlib or the soft-float routines.
//
// Note that this works on a *declaration* too, which is how
// gen_effects.py marks all the effect step/init functions without the
// effect headers having to know anything about it.
//
#define __audio_func(name) __not_in_flash("audio_" #name) name

typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;

typedef struct {
	s32 left, right;
} raw_sample_t;

typedef struct {
	float left, right;
} sample_t;

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif // _AUDIO_TYPES_H
