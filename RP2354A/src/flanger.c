// Flanger effect based on the MIT-licensed DaisySP library by Electrosmith
// which in turn seems to be based on Soundpipe by Paul Batchelor

#include "pico/stdlib.h"

#include "util.h"
#include "flanger.h"
#include "lfo.h"

// Max ~1.25s delays at ~52kHz
#define ARRAY_SIZE 65536
#define ARRAY_MASK (ARRAY_SIZE-1)
static float array[ARRAY_SIZE];
static int array_index;

static float feedback = 0.2;
static float delay = 0.0;
static float depth = 0.9;

static float target_delay = 0.0;

static struct lfo_state flanger_lfo = { .type = lfo_sinewave };

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

	if (samples > 0 && samples < ARRAY_SIZE)
		target_delay = samples;
}

static void array_write(float val)
{
	array[ARRAY_MASK & ++array_index] = val;
}

static float array_read(float delay)
{
	int i = (int) delay;
	float frac = delay - i;
	int idx = array_index - i;

	float a = array[ARRAY_MASK & idx];
	float b = array[ARRAY_MASK & ++idx];
	return a + (b-a)*frac;
}

void flanger_init(float pot1, float pot2, float pot3, float pot4)
{
	flanger_set_lfo(pot1*pot1*10);	// lfo = 0 .. 10Hz
	flanger_set_delay(pot2 * 4);	// delay = 0 .. 4 ms
	flanger_set_depth(pot3);	// depth = 0 .. 100%
	flanger_set_feedback(pot4);	// feedback = 0 .. 100%
}

#define UPDATE(x) x += 0.001 * (target_##x - x)

float flanger_step(float in)
{
	UPDATE(delay);

	float d = 1 + delay + lfo_step(&flanger_lfo) * depth * delay;
	float out;

	out = array_read(d);
	array_write(limit_value(in + out * feedback));

	return (in + out) / 2;
}

static struct lfo_state delay_lfo = { .type = lfo_sinewave };
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

	out = array_read(d);
	array_write(limit_value(in + out * feedback));

	return (in + out)/ 2;
}
