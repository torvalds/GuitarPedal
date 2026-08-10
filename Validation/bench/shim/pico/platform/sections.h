//
// Enough of the SDK's section attributes to compile the audio core on a
// workstation.
//
// On the pedal these pull code and data out of flash and into RAM, and
// the section names are what scripts/check-audio.py keys off.  Neither
// matters here: there is no XIP window to fall out of and no map file to
// check.  What matters is that audio/types.h and the effect headers can
// say __not_in_flash() and still compile.
//
#ifndef _BENCH_PICO_SECTIONS_H
#define _BENCH_PICO_SECTIONS_H

#define __not_in_flash(group)
#define __not_in_flash_func(func) func
#define __time_critical_func(func) func

#endif
