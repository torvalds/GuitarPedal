// NAME: Flanger [FLANGER]
// PRIORITY: 60
// POT: "Freq" SQUARED(0.0 10.0) = 2.5 Hz
// INFO: How fast the sweep runs, in cycles per second.
// POT: "Delay" LINEAR(0.0 4.0) = 2.0 ms
// INFO: The delay the sweep is centred on. Short is a metallic jet
// INFO: whoosh, long drifts towards chorus.
// POT: "Depth" LINEAR(0.0 1.0) = 0.5
// INFO: How far the delay swings either side of that.
// POT: "Feedback" LINEAR(0.0 1.0) = 0.5
// INFO: How much of the output goes back in. More makes it
// INFO: sharper and more resonant.
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
	set_lfo_freq(&flanger.lfo, flanger_freq_pot(pot));
	flanger.delay = flanger_delay_pot(pot) * SAMPLES_PER_MSEC;
	flanger.depth = flanger_depth_pot(pot);
	flanger.feedback = flanger_feedback_pot(pot);
}

static inline float flanger_step(float in)
{
	float d = 1 + flanger.delay * (1 + lfo_step(&flanger.lfo, lfo_sinewave) * flanger.depth);
	float out;

	out = sample_array_read(d, &flanger.idx, flanger.samples);
	sample_array_write(tanhf(in + out * flanger.feedback), &flanger.idx, flanger.samples);

	return (in + out) / 2;
}
