// NAME: Test Tone [TESTTONE]
// PRIORITY: 128
// MIX: LINEAR
// DEFAULT_MIX: 1.0
// POT: "Level" LINEAR(-90.0 0.0) = -20.0 dBFS
// INFO: How loud the tone is, as dBFS out. Fully down is digital
// INFO: silence rather than a very quiet tone, so it can be used to
// INFO: drive the next pedal's input with a terminated nothing.
// POT: "Freq" EXPONENTIAL(13.75 14080.0) = 440.0 Hz
// INFO: Ten octaves in 120 steps, so one step is a semitone and the
// INFO: middle of the range is A440 exactly.
// POT: "Shape" ENUM(Sine Triangle Saw Noise) = Sine
//
// A signal generator, in the pedal.
//
// This is a test instrument rather than an effect, and it is here because
// the bench instrument it replaces cannot be reached from a test.  The
// hardware suite in Validation/ has to be *told* what its generator is set
// to - `--ptp 0.100 --freq 440` - and every number it reports is quoted
// against that claim rather than against a measurement.  So when a capture
// comes out wrong there is no way to tell a pedal that broke from a bench
// that moved, and the noise floor it measures includes whatever the
// generator contributes, because the generator cannot be switched off.
//
// A pedal generating into another pedal fixes all three at once: the
// stimulus is set over MIDI, it can be turned off, and it is produced by
// neither the host nor the pedal being measured.
//
// It ignores its input, which is the whole point.  Two pedals patched
// output-to-input in both directions are an oscillator - noise goes round
// the loop, gains a little each time, and both of them sit there clipping.
// At full mix this replaces the input rather than adding to it, so routing
// it on one of the two breaks the loop at a known place, and does it
// through the ordinary mix control rather than through anything that had
// to be invented for it.  Hence DEFAULT_MIX: 1.0 - the useful setting is
// the whole output, and anything less is for hearing the tone alongside
// what you are playing, which is the unusual case.
//
// Priority puts it late, between the amp and the cab.  That is only where
// it lands when nothing says otherwise: routing order is set explicitly,
// so feeding a tone *into* the rest of the chain is a routing message and
// not a reason to place it early.  Late is the better default because the
// common use is measuring what comes out of the jack, and the fewer things
// between the generator and the jack the better.
//
// Two things do sit between it and the jack whatever the routing, both in
// the signal chain and neither avoidable from here: the master Volume
// scales it, and a bypassed pedal crossfades back to its input. So a test
// asserting an absolute level has to set Volume rather than assume it.
// The gate does not, being upstream of every routed effect.
//
// The shapes are the LFO's, at audio rate.  Nothing about a phase
// accumulator cares which side of 20Hz it is running on, and set_lfo_freq()
// is deliberately unclamped, so this is the existing oscillator asked for a
// bigger number - and the sine is the quarter-table with interpolation that
// every modulated effect already uses, so the tone is exactly as clean as
// the vibrato is.  The first three enum values are in lfo_type order on
// purpose; the fourth is not an LFO at all.
//
// Noise is the odd one out and is worth having: a sweep measures one
// frequency at a time and needs a run per point, while noise measures the
// whole response at once, and it is also the honest stimulus for anything
// asking how a filter behaves rather than where its corner is.  xorshift32
// rather than rand(), which is a call out of the audio sections that
// check-audio.py refuses - correctly, since it would be a jump into flash
// on every sample.  Full period, uniform rather than gaussian, and the
// state is seeded once at boot rather than at every init so that moving a
// pot does not restart the sequence.

static struct {
	struct lfo_state lfo;
	float amp;
	enum lfo_type type;
	int noise;
	u32 rng;
} testtone;

static void testtone_init(unsigned char pot[10])
{
	set_lfo_freq(&testtone.lfo, testtone_freq_pot(pot));

	//
	// Fully down is off.  The bottom of the range is -90dBFS, which is
	// twenty decibels under the quietest noise floor either board has
	// measured and so is nearly nothing - but "nearly nothing" is the
	// wrong answer for the pedal at the far end of the cable, which is
	// trying to find out how quiet it can be.  Same convention as the
	// gate's threshold, for the same reason.
	//
	testtone.amp = pot[TESTTONE_LEVEL] ?
		db_to_level(testtone_level_pot(pot)) : 0.0f;

	testtone.noise = pot[TESTTONE_SHAPE] == 3;
	testtone.type = testtone.noise ? lfo_sinewave : pot[TESTTONE_SHAPE];

	if (!testtone.rng)
		testtone.rng = 2463534242u;
}

static float testtone_step(float in)
{
	// Deliberately: a generator is not a function of its input
	(void) in;

	if (testtone.noise) {
		u32 x = testtone.rng;

		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		testtone.rng = x;

		return testtone.amp * (2.0f * u32_to_fraction(x) - 1.0f);
	}

	float v = lfo_step(&testtone.lfo, testtone.type);

	//
	// The saw comes back as 0 to 1 where the other two come back as -1
	// to 1 - it is the raw phase, which is what makes it useful to the
	// tremolo and useless to us.  Centring it here rather than in
	// lfo.h, because that range is the point of it there.
	//
	if (testtone.type == lfo_sawtooth)
		v = 2.0f * v - 1.0f;

	return testtone.amp * v;
}
