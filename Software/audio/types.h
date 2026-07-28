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

//
// One-way barriers, kernel style.
//
// 'smp_store_release()' makes everything written before it visible to
// the other cpu before the store itself becomes visible.  The matching
// 'smp_load_acquire()' makes everything read after it happen after the
// load.  That is exactly the pattern this code keeps needing: fill a
// buffer and then release the index, acquire the index and then read
// the buffer.
//
// On armv8-m these are a single 'stl' and 'lda', so they cost nothing
// over the plain accesses they replace - and rather less than the
// 'volatile' they replace, which forced reloads for no ordering.
//
#define smp_store_release(p, v)	__atomic_store_n(p, v, __ATOMIC_RELEASE)
#define smp_load_acquire(p)	__atomic_load_n(p, __ATOMIC_ACQUIRE)

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
