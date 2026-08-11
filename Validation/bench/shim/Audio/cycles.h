//
// No cycle counter here, and nothing on this side wants one.
//
// The bench runs the pedal's own single_sample() to find out what an
// effect does to a *signal*.  What it costs a Cortex-M33 is a different
// question, and it is answered on the hardware by measure-load.py off
// the pedal's telemetry - a workstation's answer to that would be worse
// than none, because it would look like a measurement.
//
// So: a counter that advances, which is all the load meter's arithmetic
// needs to stay well defined, and which makes it settle at a flat zero
// rather than at something plausible.  The real one is in
// Software/audio/cycles.h and says why it has to be reachable from
// here.
//
#ifndef AUDIO_CYCLES_H
#define AUDIO_CYCLES_H

static inline void cycle_counter_init(void)
{
}

static inline uint32_t cycle_count(void)
{
	static uint32_t fake;

	return fake += 1000;
}

#endif
