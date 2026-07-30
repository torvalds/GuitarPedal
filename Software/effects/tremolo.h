// NAME: Tremolo [TREM]
// PRIORITY: 110
// MIX: STEREO		// NORM pans, so it needs somewhere to pan *to*
// POT: "Rate" FREQUENCY(0.1 10.0) = 2.5 Hz
// POT: "Depth" LINEAR(0.0 1.0) = 0.5
// POT: "Mode" ENUM(NORM HARM) = NORM
// Tremolo: sinusoidal amplitude modulation, NORM and HARM modes.
//
// NORM: a rotation of the stereo signal, done as a complex multiply.
//   Take the input as l + i*r and multiply by
//
//       m = (1 - d) + d * (cos φ + i * sin φ)
//
//   where φ is the LFO phase running freely round the circle and d is
//   depth. At d = 0 the multiplier is 1 and nothing happens at all. At
//   d = 1 it is a pure unit rotation, so |out| == |in| exactly - equal
//   power by construction, for a mono input and a stereo one alike,
//   with no mix law needed to arrange it. In between, m traces a circle
//   offset from 1, so the magnitude varies as well as the angle and you
//   get amplitude modulation and panning together, which is what a
//   stereo tremolo sounds like.
//
//   A mono input comes out in quadrature - l*cos on the left, l*sin on
//   the right - both moving at the LFO rate, ninety degrees apart. Fold
//   that back to mono and you get sqrt(2)*l*sin(φ + π/4), i.e. it
//   degrades to a full-depth ordinary tremolo rather than cancelling to
//   nothing, which is what an opposite-signs-per-channel version would
//   have done.
//
//   At full depth the rotation passes through φ = π, where the left
//   channel is -l. Going all the way round means inverting on the way,
//   which is inherent to rotating rather than a fault in it.
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
	float depth;	// NORM: how far the multiplier moves off 1
	float k;	// HARM: bipolar AM coefficient
	int harmonic;
	float lp1_a, lp1_z;  // one-pole LP at 144.7 Hz (low branch)
	float lp2_a, lp2_z;  // one-pole LP at 636.6 Hz; HP = in - lp2 (high branch)
} tremolo;

static void tremolo_init(unsigned char pot[10])
{
	set_lfo_freq(&tremolo.lfo, tremolo_pot0(pot[0]));
	float d = tremolo_pot1(pot[1]);
	tremolo.depth = d;
	tremolo.k = d / (2.0f - d);
	tremolo.harmonic = (pot[2] == 1);

	tremolo.lp1_a = pow2(-9.06472028f * 144.7f / SAMPLES_PER_SEC);
	tremolo.lp2_a = pow2(-9.06472028f * 636.6f / SAMPLES_PER_SEC);
}

static sample_t tremolo_step(sample_t in)
{
	sample_t out;

	if (tremolo.harmonic) {
		// Still mono: the 6G4 circuit this models has one signal
		// path, and the two branches are filters, not channels.
		float lfo = lfo_step(&tremolo.lfo, lfo_sinewave);
		float x = in.left;

		float lo = (tremolo.lp1_z = x + tremolo.lp1_a * (tremolo.lp1_z - x));
		float hi = x - (tremolo.lp2_z = x + tremolo.lp2_a * (tremolo.lp2_z - x));
		out.left = out.right = x + tremolo.k * lfo * (lo - hi);
		return out;
	}

	// The sawtooth is the raw phase, which is what we want here -
	// fastsincos() takes its phase in cycles, so the two line up
	// without any scaling in between.
	struct sincos w = fastsincos(lfo_step(&tremolo.lfo, lfo_sawtooth));
	float d = tremolo.depth;
	float re = 1.0f + d * (w.cos - 1.0f);
	float im = d * w.sin;

	out.left = in.left * re - in.right * im;
	out.right = in.left * im + in.right * re;
	return out;
}
