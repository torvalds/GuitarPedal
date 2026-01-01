// Flanger effect based on the MIT-licensed DaisySP library by Electrosmith
// which in turn seems to be based on Soundpipe by Paul Batchelor

#include "pico/stdlib.h"

#include "util.h"
#include "flanger.h"
#include "lfo.h"

static float feedback;
static float delay, target_delay;
static float depth;
static struct lfo_state flanger_lfo;

static inline void flanger_set_lfo(float f)
{
	set_lfo_freq(&flanger_lfo, f);
}

static inline void flanger_set_depth(float d)
{
	if (d > 0 && d < 1)
		depth = d;
}

static inline void flanger_set_feedback(float fb)
{
	if (fb >= 0 && fb < 1)
		feedback = fb;
}

static inline void flanger_set_delay(float ms)
{
	float samples = ms * (SAMPLES_PER_SEC * 0.001);

	if (samples > 0 && samples < SAMPLE_ARRAY_SIZE)
		target_delay = samples;
}

void flanger_init(float pot1, float pot2, float pot3, float pot4)
{
	flanger_set_lfo(pot1*pot1*10);	// lfo = 0 .. 10Hz
	flanger_set_delay(pot2 * 4);	// delay = 0 .. 4 ms
	flanger_set_depth(pot3);	// depth = 0 .. 100%
	flanger_set_feedback(pot4);	// feedback = 0 .. 100%
}

// To avoid crackling, the low-level delay
// must not change abruptly when the pot is
// turned (or when the pot value just randomly
// fluctuates).
//
// So we take the requested delay as a target
// that we approach smoothly
#define UPDATE(x) x += 0.001 * (target_##x - x)

float flanger_step(float in)
{
	UPDATE(delay);

	float d = 1 + delay * (1 + lfo_step(&flanger_lfo) * depth);
	float out;

	out = sample_array_read(d);
	sample_array_write(limit_value(in + out * feedback));

	return (in + out) / 2;
}

static struct lfo_state delay_lfo;
static float delay_feedback;

void delay_init(float pot1, float pot2, float pot3, float pot4)
{
	flanger_set_delay(pot1 * 1000);	// delay = 0 .. 1s
	set_lfo_ms(&delay_lfo, pot3*4);	// LFO = 0 .. 4ms
	delay_feedback = pot4;		// feedback = 0 .. 100%
}

float delay_step(float in)
{
	UPDATE(delay);

	float d = 1 + delay;
	float out;

	out = sample_array_read(d);
	sample_array_write(limit_value(in + out * delay_feedback));

	return (in + out)/ 2;
}
