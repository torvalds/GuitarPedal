// NAME: Flanger [FLNGR]
// PRIORITY: 60
// POT: "Freq" SQUARED(0.0 10.0) = 2.5 Hz
// POT: "Delay" LINEAR(0.0 4.0) = 2.0 ms
// POT: "Depth" LINEAR(0.0 1.0) = 0.5
// POT: "Feedback" LINEAR(0.0 1.0) = 0.5
// Flanger effect based on the MIT-licensed DaisySP library by Electrosmith
// which in turn seems to be based on Soundpipe by Paul Batchelor

static struct {
	struct lfo_state lfo;
	float delay, depth, feedback;

	// Large enough history buffer for 10ms
	unsigned int idx;
	float samples[1024];
} flanger;

static inline void flanger_init(unsigned char pot[10])
{
	set_lfo_freq(&flanger.lfo, flanger_pot0(pot[0]));
	flanger.delay = flanger_pot1(pot[1]) * SAMPLES_PER_MSEC;
	flanger.depth = flanger_pot2(pot[2]);
	flanger.feedback = flanger_pot3(pot[3]);
}

static inline float flanger_step(float in)
{
	float d = 1 + flanger.delay * (1 + lfo_step(&flanger.lfo, lfo_sinewave) * flanger.depth);
	float out;

	out = sample_array_read(d, &flanger.idx, flanger.samples);
	sample_array_write(tanhf(in + out * flanger.feedback), &flanger.idx, flanger.samples);

	return (in + out) / 2;
}
