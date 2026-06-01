// NAME: Tremolo [TREM]
// PRIORITY: 110
// POT: "Rate" FREQUENCY(0.1 10.0) = 2.5 Hz
// POT: "Depth" LINEAR(0.0 1.0) = 0.5
// POT: "Mode" ENUM(NORM HARM) = NORM
// Tremolo: sinusoidal amplitude modulation, NORM and HARM modes.
//
// NORM: gain = 1 + k * lfo, where k = depth / (2 - depth).
//   Average gain is exactly 1.0 at all depth settings. At 100% depth,
//   gain swings 0..2; at 50% it swings ~0.67..1.33. Bipolar AM formula
//   (Strymon/Boss-class) rather than one-sided 0..1, which would lose up
//   to 6 dB of average level at full depth.
//
// HARM: two independent one-pole filters modelled on the Fender 6G4 harmonic
//   vibrato circuit. Low branch is a 1-pole LP (R=220k, C=5nF, fc≈144.7 Hz);
//   high branch is a 1-pole HP (R=1M, C=250pF, fc≈636.6 Hz, implemented as
//   in - LP at that same corner). These are not complementary crossover halves,
//   so lo+hi has a broad mid-frequency dip (~8 dB at 300 Hz). To avoid that
//   level drop the LFO modulates the difference (lo - hi) added to in, not the
//   branches directly: out = in + k*lfo*(lo - hi). Unity gain at zero depth,
//   same anti-phase modulation character at non-zero depth.

static struct {
	struct lfo_state lfo;
	float k;
	int harmonic;
	float lp1_a, lp1_z;  // one-pole LP at 144.7 Hz (low branch)
	float lp2_a, lp2_z;  // one-pole LP at 636.6 Hz; HP = in - lp2 (high branch)
} tremolo;

static void tremolo_init(unsigned char pot[10])
{
	set_lfo_freq(&tremolo.lfo, tremolo_pot0(pot[0]));
	float d = tremolo_pot1(pot[1]);
	tremolo.k = d / (2.0f - d);
	tremolo.harmonic = (pot[2] == 1);

	tremolo.lp1_a = pow2(-9.06472028f * 144.7f / SAMPLES_PER_SEC);
	tremolo.lp2_a = pow2(-9.06472028f * 636.6f / SAMPLES_PER_SEC);
}

static float tremolo_step(float in)
{
	float lfo = lfo_step(&tremolo.lfo, lfo_sinewave);

	if (!tremolo.harmonic)
		return in * (1.0f + tremolo.k * lfo);

	float lo = (tremolo.lp1_z = in + tremolo.lp1_a * (tremolo.lp1_z - in));
	float hi = in - (tremolo.lp2_z = in + tremolo.lp2_a * (tremolo.lp2_z - in));
	return in + tremolo.k * lfo * (lo - hi);
}
