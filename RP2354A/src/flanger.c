// Flanger effect based on the MIT-licensed DaisySP library by Electrosmith
// which in turn seems to be based on Soundpipe by Paul Batchelor

#define SAMPLES_PER_SEC 48000
#define SAMPLES_PER_MSEC 48

// Max ~1.3s delays at 48kHz
#define ARRAY_SIZE 65536
#define ARRAY_MASK (ARRAY_SIZE-1)
static float array[ARRAY_SIZE];
static int array_index;

static float lfo_phase = 0.0;
static float lfo_freq = 0.1;

static float feedback = 0.2;
static float delay = 0.75 * SAMPLES_PER_MSEC;	// 0.75ms in samples
static float depth = 0.9;

static float target_delay = 0.75 * SAMPLES_PER_MSEC;

// Very random - but quick - function to smoothly
// limit x to -1 .. 1 when it approaches -2..2
//
// So you can add two values in the -1..1 range and
// then limit the sum to that range too.
static float limit_value(float x)
{
	float x2 = x*x;
	float x4 = x2*x2;
	return x*(1 - 0.19*x2 + 0.0162*x4);
}

void flanger_set_lfo(float f)
{
	if (f < 0)
		return;
	f = f * (4.0 / SAMPLES_PER_SEC);
	if (f > 0.25)
		f = 0.25;
	if (lfo_freq < 0)
		f = -f;
	lfo_freq = f;
}

void flanger_set_depth(float d)
{
	if (d > 0 && d < 1)
		depth = d;
}

void flanger_set_feedback(float fb)
{
	if (fb >= 0 && fb < 1)
		feedback = fb;
}

void flanger_set_delay(float ms)
{
	float samples = ms * SAMPLES_PER_MSEC;

	if (samples > 0 && samples < ARRAY_SIZE)
		target_delay = samples;
}

static void array_write(float val)
{
	array[ARRAY_MASK & ++array_index] = val;
}

static float array_read(float delay)
{
	int i = delay;
	int frac = delay - i;
	int idx = array_index - i;

	float a = array[ARRAY_MASK & idx];
	float b = array[ARRAY_MASK & ++idx];
	return a + (b-a)*frac;
}

/* LFO is a triangle wave from 0..2 at lfo_freq per sample */
static float flanger_lfo_step(void)
{
	lfo_phase += lfo_freq;
	if (lfo_phase > 2) {
		lfo_freq = -lfo_freq;
		lfo_phase = 4 - lfo_phase;
	} else if (lfo_phase < 0) {
		lfo_freq = -lfo_freq;
		lfo_phase = -lfo_phase;
	}
	return lfo_phase;
}

#define UPDATE(x) x += 0.001 * (target_##x - x)

float flanger_step(float in)
{
	UPDATE(delay);

	float d = 1 + flanger_lfo_step() * depth * delay;
	float out;

	out = array_read(d);
	array_write(limit_value(in + out * feedback));

	return (in + out) / 2;
}
