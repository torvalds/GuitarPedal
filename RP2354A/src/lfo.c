//
// Our LFO generates a quarter cycle (0 .. 1) from a 30-bit cycle
// and when it overflows it changes the quarter counter, which
// then turns 0..1 into a series of [ 0..1 , 1..0 , 0..-1, -1..0 ]
//
// Every audio cycle (48kHz), we update the LFO counter
// by 'lfo_step', so the cycle of one quarter is
//
//	t = 1/48000 * 2**30 / lfo_step
//
// and a full cycle is four times that (ie the full 32-bit cycle).
//
// Calling that (2**32)/48k "F_STEP", we get
//
//	T = F_STEP / lfo_step
//	freq = lfo_step / F_STEP
//	 	=> lfo_step = freq * F_STEP
//	ms = 1000 * T = 1000 * F_STEP / lfo_step
//		=> lfo_step = 1000 * F_STEP / ms
//
#define TWO_POW_32 (4294967296.0f)
#define F_STEP (TWO_POW_32/48000.0)

#include <math.h>
#include "pico/stdlib.h"
#include "gensin.h"
#include "lfo.h"

//
// The quarter information is naturally in the two high
// bits of the index
//
static uint lfo_idx;
static uint lfo_idx_step;
static enum lfo_type lfo_type = lfo_triangle;

static inline float uint_to_fraction(uint val)
{
	return (1.0/TWO_POW_32) * val;
}

static inline uint fraction_to_uint(float val)
{
	return val * TWO_POW_32;
}

static inline void set_lfo_step(float step)
{
	lfo_idx_step = rintf(step);
}

void set_lfo_freq(float freq)
{
	set_lfo_step(freq * F_STEP);
}

void set_lfo_ms(float ms)
{
	if (ms < 0.1)
		ms = 0.1;
	set_lfo_step(1000 * F_STEP / ms);
}

void set_lfo_type(enum lfo_type type)
{
	lfo_type = type;
}

float lfo_step(void)
{
	uint now = lfo_idx;
	uint next = now + lfo_idx_step;

	lfo_idx = next;

	if (lfo_type == lfo_sawtooth)
		return uint_to_fraction(now);

	float val;
	uint quarter = now >> 30;
	now <<= 2;

	// Second and fourth quarter reverses direction
	if (quarter & 1)
		now = ~now;

	if (lfo_type == lfo_sinewave) {
		uint idx = now >> (32-QUARTER_SINE_STEP_SHIFT);
		float a = quarter_sin[idx];
		float b = quarter_sin[idx+1];

		now <<= QUARTER_SINE_STEP_SHIFT;
		val = a + (b-a)*uint_to_fraction(now);
	} else {
		val = uint_to_fraction(now);
	}

	// Last two quarters are negative
	if (quarter & 2)
		val = -val;
	return val;
}
