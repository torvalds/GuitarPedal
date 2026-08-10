//
// The one timer register the audio core reads.
//
// single_sample() times its own spin one sample in sixteen to work out
// the cpu load.  That number is not audio and nothing here checks it, so
// a counter that advances by one nominal sample period per read is
// enough to keep the arithmetic in range and meter_load plausible.
//
#ifndef _BENCH_HARDWARE_TIMER_H
#define _BENCH_HARDWARE_TIMER_H

#include <stdint.h>

struct bench_timer {
	uint32_t timerawl;
};

extern struct bench_timer bench_timer_regs;

#define timer_hw (&bench_timer_regs)

#endif
