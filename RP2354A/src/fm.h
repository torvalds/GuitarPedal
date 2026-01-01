//
// Silly frequency modulation signal generator "effect"
// It doesn't actually care about the input, it's useful
// mainly for testing the LFO
//
static struct lfo_state base_lfo, modulator_lfo;
static float fm_volume, fm_base_freq, fm_freq_range;

static void fm_init(float pot1, float pot2, float pot3, float pot4)
{
	fm_volume = pot1;
	fm_base_freq = fastpow(8000, pot2)+100;
	fm_freq_range = pot3;				//  max range one octave down and up
	set_lfo_freq(&modulator_lfo, 1 + 10*pot4);	// 1..11 Hz
}

static float fm_step(float in)
{
	float multiplier = fastpow2_m1(lfo_step(&modulator_lfo) * fm_freq_range) + 1;
	float freq = fm_base_freq * multiplier;
	set_lfo_freq(&base_lfo, freq);
	return lfo_step(&base_lfo) * fm_volume;
}
