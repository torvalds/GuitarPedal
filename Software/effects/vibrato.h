// Vibrato: LFO-modulated delay line for a classic Doppler shift.
// Blending dry signal with the wet signal produces a rich chorus-like effect.
// At 100% wet only the pitch-shifted signal is audible; fun for rotary speaker emulation.

#define VIBRATO_CENTER_SAMPLES (6.0f * SAMPLES_PER_MSEC)

static struct {
	struct lfo_state lfo;
	float depth;		// delay amplitude in samples
	float dry, wet;		// equal-power gain coefficients
	unsigned int idx;
	float samples[1024];	// ~21ms at 48kHz; max read = center(6ms) + depth(5ms) = 528 samples
} vibrato;

static float vibrato_rate(signed char pot)  { return frequency_pot(pot, 0.1f, 8.0f); }
static float vibrato_depth(signed char pot) { return linear_pot(pot, 0.0f, 5.0f); }
static float vibrato_mix(signed char pot)   { return POT_TO_FLOAT(pot); }

static void vibrato_init(signed char pot[10])
{
	set_lfo_freq(&vibrato.lfo, vibrato_rate(pot[0]));
	vibrato.depth = vibrato_depth(pot[1]) * SAMPLES_PER_MSEC;

	struct sincos w = fastsincos(0.25f * vibrato_mix(pot[2]));
	vibrato.dry = w.cos;
	vibrato.wet = w.sin;
}

static float vibrato_step(float in)
{
	float d = VIBRATO_CENTER_SAMPLES + vibrato.depth * lfo_step(&vibrato.lfo, lfo_sinewave);
	sample_array_write(in, &vibrato.idx, vibrato.samples);
	float wet = sample_array_read(d, &vibrato.idx, vibrato.samples);
	return vibrato.dry * in + vibrato.wet * wet;
}

static struct effect vibrato_effect = {
	.name = "Vibrato",
	.short_name = "VIB",
	.init = vibrato_init,
	.step = vibrato_step,
	.pots = {
		EFFECT_POT("Rate",  desc_Hz,   vibrato_rate,  15,  NULL ),
		EFFECT_POT("Depth", desc_ms,   vibrato_depth, -39, NULL ),
		EFFECT_POT("Mix",   desc_none, vibrato_mix,   60, NULL ),
	}
};
