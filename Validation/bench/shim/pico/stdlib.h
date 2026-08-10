//
// The one SDK idiom the audio core uses outside of the two register
// blocks: the hint that a spin loop is a spin loop.  On the pedal it is
// a nop the compiler is told not to remove; here the spin never spins,
// because the bench has always filled the slot before calling in.
//
#ifndef _BENCH_PICO_STDLIB_H
#define _BENCH_PICO_STDLIB_H

#define tight_loop_contents() ((void)0)

#endif
