//
// The cpu's own cycle counter.
//
// The audio load meter times its idle spin with this: a microsecond is
// 4.8% of a 20.83us sample period and a cycle at 153.6MHz is 0.03%, so
// the difference is between an instrument that can see one effect and
// one that cannot.
//
// A file of its own, and included as "audio/cycles.h" rather than
// "cycles.h", because that is what makes it shimmable.  A quoted
// include searches the including file's own directory first, so
// "cycles.h" from audio/effect.h would always find this one; spelling
// the directory out sends it round the -I list instead, where
// Validation's bench puts its shim ahead of the tree.
//
// It needs to be shimmable because these are raw addresses in the M33's
// private peripheral bus.  On the pedal they are the DWT; on a
// workstation running the same single_sample(), dereferencing
// 0xE0001004 is a segfault rather than a measurement - which is exactly
// what happened, and is why this file exists.
//
#ifndef AUDIO_CYCLES_H
#define AUDIO_CYCLES_H

#define DWT_DEMCR	(*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL	(*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT	(*(volatile uint32_t *)0xE0001004u)

//
// Per-core, and the whole DWT is off until TRCENA is set - so this has
// to run on the core that reads it, which is core 1.
//
static inline void cycle_counter_init(void)
{
	DWT_DEMCR |= 1u << 24;		// TRCENA
	DWT_CTRL  |= 1u << 0;		// CYCCNTENA
}

static inline uint32_t cycle_count(void)
{
	return DWT_CYCCNT;
}

#endif
