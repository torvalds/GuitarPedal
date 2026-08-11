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

//
// SLOW LFOs, WHICH ARE MOST OF THEM
//
// Running the quarter-sine lookup 48000 times a second to describe
// something whose period is a couple of seconds is silly, and it is
// measurably expensive: on the RP2354 the reverb's four LFOs cost about
// 11% of the whole effect.  That expense is exactly why the reverb
// arrived here carrying its own hand-rolled rotator instead, which then
// spent thirteen hours walking out of its buffer.
//
// So: do the real thing every LFO_X samples, and walk a straight line
// between those points.  The error is the sagitta of a sine over the
// span and grows as LFO_X squared - at 32 it is 120dB down for the
// reverb's 0.67Hz, 73dB down at a 10Hz tremolo and 49dB down for the
// fastest thing here, a 40Hz phaser.
//
// WHY 32, AND WHY IT IS NOT A PARAMETER
//
// Per-sample work is one add plus 1/X of the real computation, so the
// cost falls towards the cost of that one add and then stops.  32 is
// already there: measured on hardware, X=256 came out 0.64 telemetry
// steps cheaper than X=32, which is inside the noise, while costing
// 36dB of accuracy.  Past the knee, so a knob for it would only be a
// way to get it wrong.
//
// WHICH LFOs THIS IS NOT FOR
//
// testtone.h drives lfo_step() up to 14kHz on purpose - the shapes are
// the LFO's, at audio rate, and set_lfo_freq() is deliberately
// unclamped.  Chording that would be nonsense.  Hence a separate call
// rather than a change to lfo_step(): the fast path stays exact and
// asking for the cheap one is a decision at the call site.
//
// THE COUNTER IS THE CHAIN'S, NOT THE EFFECT'S
//
// single_sample() increments this once a frame and nothing else touches
// it.  Having one counter rather than one per LFO is what lets the
// compiler see that four lfo_step_X() calls in a row share a test, and
// it means an effect cannot get its own phase wrong.  It is allowed to
// wrap; only the low bits are ever read.
//
unsigned audio_sample_count;

#define LFO_X_BITS	5
#define LFO_X		(1u << LFO_X_BITS)
#define LFO_X_MASK	(LFO_X - 1)

struct lfo_slow {
	struct lfo_state lfo;
	float value, slope;
};

//
// The step is LFO_X times bigger because it is applied LFO_X times less
// often.  Same accumulator, same wrap, same everything else.
//
static inline void set_lfo_freq_X(struct lfo_slow *s, float freq)
{
	set_lfo_step(&s->lfo, freq * F_STEP * LFO_X);
}

//
// A new frequency takes effect at the next recalculation rather than at
// once, which is a property of the model rather than a wart in it: the
// value is mid-chord and the chord it is on was costed before the knob
// moved.  Worst case is LFO_X samples of the old rate, which is 667us.
//
// Nothing is seeded here or at init.  value and slope start at zero, so
// the first block walks from zero up to wherever the LFO actually is -
// a 667us fade in on a modulator, which is not worth code to avoid.
//
static inline float lfo_step_X(struct lfo_slow *s, enum lfo_type type)
{
	if (!(audio_sample_count & LFO_X_MASK)) {
		float next = lfo_step(&s->lfo, type);
		s->slope = (next - s->value) * (1.0f / LFO_X);
	}
	return (s->value += s->slope);
}
