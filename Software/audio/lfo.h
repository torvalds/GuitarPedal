//
// Our LFO generates a quarter cycle (0 .. 1) from a 30-bit cycle
// and when it overflows it changes the quarter counter, which
// then turns 0..1 into a series of [ 0..1 , 1..0 , 0..-1, -1..0 ]
//
// The quarter information is naturally in the two high
// bits of the index
//
// Every audio cycle we update the LFO counter by 'lfo_step',
// so the cycle of one quarter is
//
//     t = 2**30 / SAMPLES_PER_SEC / lfo_step
//
// and a full cycle is four times that (ie the full 32-bit cycle).
//
// Calling that (2**32)/SAMPLES_PER_SEC "F_STEP", we get
//
//     T = F_STEP / lfo_step
//     freq = lfo_step / F_STEP
//             => lfo_step = freq * F_STEP
//     ms = 1000 * T = 1000 * F_STEP / lfo_step
//             => lfo_step = 1000 * F_STEP / ms
//
#define F_STEP (TWO_POW_32/SAMPLES_PER_SEC)

//
// THE THREE SHAPES DO NOT SHARE A RANGE.  Sine and triangle are bipolar,
// -1 to 1, because that is what a modulator added to a centre value
// wants.  The sawtooth is 0 to 1, and that is deliberate: it is the raw
// phase, so it can be handed straight to something that takes a phase in
// cycles.  tremolo.h feeds it to fastsincos() and gets its rotation for
// free, with no scaling in between and no wrap to get wrong.
//
// The asymmetry earns its keep, so it stays - but it is worth what it
// costs only if it is known about, and it was not written down anywhere
// until a test tone read the shape number straight out of this enum and
// produced a saw with half its level in DC.  An audio waveform wants
// 2*v - 1; a phase does not.  Ask which one you are asking for.
//
enum lfo_type {
	lfo_sinewave,		// -1 .. 1
	lfo_triangle,		// -1 .. 1
	lfo_sawtooth,		//  0 .. 1  - raw phase, see above
};

struct lfo_state {
	u32 idx, step;
};

// Use this for LFO initializers.
#define LFO_FREQ(x) .step = (x)*F_STEP

static inline void set_lfo_step(struct lfo_state *lfo, float step)
{
	lfo->step = (u32) rintf(step);
}

void set_lfo_freq(struct lfo_state *lfo, float freq)
{
	set_lfo_step(lfo, freq * F_STEP);
}

void set_lfo_ms(struct lfo_state *lfo, float ms)
{
	// Max 10kHz
	if (ms < 0.1)
		ms = 0.1;
	set_lfo_step(lfo, 1000 * F_STEP / ms);
}

//
// Marked, rather than trusting it to inline.  It always had, because
// every effect called it exactly once - and the first one to call it
// twice turned it into a real call and a veneer out of the audio
// sections, which check-audio.py then refused.
//
float __audio_func(lfo_step)(struct lfo_state *lfo, enum lfo_type type)
{
	u32 now = lfo->idx;
	u32 next = now + lfo->step;

	lfo->idx = next;

	if (type == lfo_sawtooth)
		return u32_to_fraction(now);

	float val;
	u32 quarter = now >> 30;
	now <<= 2;

	// Second and fourth quarter reverses direction
	if (quarter & 1)
		now = ~now;

	if (type == lfo_sinewave) {
		u32 idx = now >> (32-QUARTER_SINE_STEP_SHIFT);
		float a = quarter_sin[idx];
		float b = quarter_sin[idx+1];

		now <<= QUARTER_SINE_STEP_SHIFT;
		val = a + (b-a)*u32_to_fraction(now);
	} else {
		val = u32_to_fraction(now);
	}

	// Last two quarters are negative
	if (quarter & 2)
		val = -val;
	return val;
}
