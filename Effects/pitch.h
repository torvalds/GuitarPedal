// NAME: Pitch [PITCH]
// PRIORITY: 100
// MIX: POWER		// shifted, so it decorrelates almost immediately
// POT: "Octave" LINEAR(-2.0 2.0) = 1.0
// POT: "Feedback" LINEAR(0.0 1.0) = 0.5
// DEFAULT_MIX: 0.5
//
// Entirely random pitch shifting effect walking the
// sample buffer at varying speeds, and hiding the
// discontinuities in the sequence by picking two
// different delays, and multiplying them with sin/cos
//
// sin/cos are zero at the respective discontinuities,
// and the signal power is proportional to the square
// of the voltage. With sin^2 + cos^2 = 1 the two tap
// *powers* add to unity - but the power of a sum is
// the sum of the powers only for uncorrelated taps,
// and both taps read the same tape, so the cross term
// modulates the output level at the crossfade rate.
// pitch_step() measures that term and undoes it.
//

#define DISCONT_SHIFT 12
#define DISCONT_STEPS (1 << DISCONT_SHIFT)

struct {
	float step, feedback;
	unsigned phase, idx;
	float p1, p2, p12;	// smoothed r1^2, r2^2, r1*r2
	float follow;		// smoothing coefficient for the three
	float array[4*DISCONT_STEPS];
} pitch;

static void pitch_init(unsigned char pot[10])
{
	// Which direction do we walk the samples?
	// Walking backwards lowers the pitch
	// Walking forwards raises the pitch
	// Staying at the same delay keeps the pitch the same
	//
	float step = pow2(pitch_octave_pot(pot));	//  0.25 .. 4
	pitch.step = step - 1;				// -0.75 .. 3
	pitch.feedback = pitch_feedback_pot(pot);

	// Long enough to average the carrier out of the moment products
	// (their ripple sits at twice the note, ~26dB down at 165Hz),
	// short enough to follow a note change well inside the crossfade
	// window.  This only has to be right about what is playing, never
	// about when: the weights enter the prediction directly below.
	pitch.follow = time_constant(20.0f);
}

// i is discontinuous when sin**2 is 0
// ni is discontinuous when cos**2 (aka 1-sin**2) is 0
static float pitch_step(float in)
{
	const u32 mask = DISCONT_STEPS-1;
	u32 phase = pitch.phase++;

	u32 i = phase & mask;
	u32 ni = (i + DISCONT_STEPS/2) & mask;

	// The 31 is because we only use half the phase,
	// so sin walks 0..0.5 and cos walks 0.25..0.75
	phase <<= 31-DISCONT_SHIFT;
	struct sincos w = fastsincos(u32_to_fraction(phase));

	float step = pitch.step;
	float delay = (step > 0) ? DISCONT_STEPS*step : 1;

	float r1 = sample_array_read(delay - i*step, &pitch.idx, pitch.array);
	float r2 = sample_array_read(delay - ni*step, &pitch.idx, pitch.array);

	float out = r1*w.sin + r2*w.cos;

	//
	// sin^2 + cos^2 == 1 makes this unity gain only for uncorrelated
	// taps.  These two read the same tape 2048*|step| samples apart,
	// so for a tone at f the power picks up a cross term of
	// 2*w.sin*w.cos*rho(f), rho being the correlation at that spacing,
	// and the level sweeps between 1-rho and 1+rho of itself at the
	// crossfade rate.  That is the tremolo, and it bottoms out at
	// silence wherever the tap spacing is a whole number of periods -
	// including step 0, where both taps read the same sample and the
	// weights cancel outright.
	//
	// So keep the three second moments of the raw reads, predict the
	// power of the sum from them and the current weights, and divide
	// the mean tap power back in.  The gain is a ratio of two
	// quadratics in the weights, so the signal level never enters it:
	// this rides the crossfade ripple and nothing else, and it
	// relaxes to unity wherever the taps really are uncorrelated.
	//
	// Capped at +24dB: the exact answer is unbounded where the taps
	// cancel outright, and chasing the last of that dip buys boosting
	// whatever noise is there instead.
	//
	float p1 = pitch.p1 = linear(pitch.follow, r1*r1, pitch.p1);
	float p2 = pitch.p2 = linear(pitch.follow, r2*r2, pitch.p2);
	float p12 = pitch.p12 = linear(pitch.follow, r1*r2, pitch.p12);

	float mean = 0.5f*(p1+p2);
	float predicted = w.sin*w.sin*p1 + w.cos*w.cos*p2 + 2*w.sin*w.cos*p12;

	float gain = 16.0f;
	if (predicted > mean/(16.0f*16.0f))
		gain = sqrtf(mean/predicted);

	float shaped = out*gain;

	//
	// The shaped signal goes back into the line rather than the raw
	// sum: with the feedback pot up, a modulated write keeps the
	// crossfade term alive in what the taps read next, and the
	// correction chases it forever.  Shaped has the same short-term
	// power the raw sum averages, so the loop gain is what it always
	// was, minus the part that was doing the wobbling.
	//
	sample_array_write(linear(pitch.feedback, in, shaped), &pitch.idx, pitch.array);

	return shaped;
}
