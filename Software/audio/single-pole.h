//
// Simple single-pole helpers
//
// This uses trivial wrapper structures for type safety (and
// incidentally to look a bit like the biquad code).
//
// Precompute 'alpha' - at init, in a struct - rather than calling the
// constructors from a step function.
//
// This used to say the opposite: that for a constant frequency or time
// it was better to build the coefficient inline and let the compiler
// fold it away.  That was true of the approximation these used to use,
// which was pure arithmetic on a constant.  It is not true of the exact
// form below, which goes through pow2() - a table lookup that no
// compiler can fold - so an inline call is now a real function call on
// the audio core, once per sample, forever.

struct single_pole_state { float y; };
struct single_pole_coeff { float alpha; };

// Use this if you want to precompute the coefficient
// and just have a single unified struct
struct single_pole {
	struct single_pole_coeff coeff;
	struct single_pole_state state;
};

static inline float _single_pole_step(float in,
	struct single_pole_state *state,
	const struct single_pole_coeff coeff)
{
	return state->y = linear(coeff.alpha, state->y, in);
}

//
// The coefficient that puts the -3dB point exactly where it was asked
// for.  Solving |H| = 1/sqrt(2) for the pole radius R = 1-alpha gives a
// quadratic, and the root inside the unit circle is
//
//	R = (2 - cos w) - sqrt((2 - cos w)^2 - 1)
//
// which is exact at every frequency by construction rather than close
// at some of them.
//
// This was 'omega/(1+omega)', the first term of the series for
// 1-exp(-omega), described as a fast approximation for use far away
// from Nyquist.  Which it is; the trouble was that nothing said where
// "far away" stopped, and two call sites were well inside the region
// where it does not hold.  Solved for where the pole actually landed:
//
//	asked      got     error		asked      got     error
//	   30    29.9Hz    -0.2%		 5000  3930.4Hz   -21.4%
//	  150   148.6Hz    -1.0%		 9000  6273.0Hz   -30.3%
//	 1000   940.9Hz    -5.9%		15000  9272.2Hz   -38.2%
//	 3000  2554.0Hz   -14.9%		20000 11596.4Hz   -42.0%
//
// klon.h asked for a 15kHz input bandwidth and measured 8.8kHz through
// the effect; frenchie.h's tone control asked for 1500..20000Hz and
// swept 1373..11596, so the top half of that knob was two dull
// settings.  Both are audible and neither was a deliberate voicing.
//
// NOTE! The obvious fix is 1-exp(-2*pi*fc/fs), which is what echo.h,
// preamp.h and tremolo.h all use inline, and it is *not* what this
// does.  That mapping matches an RC's decay rather than its -3dB point,
// and the two part company near Nyquist - measured at the asked-for
// frequency, where -3.01dB is the right answer:
//
//	asked     old    1-exp()   this
//	  30    -3.02     -3.01   -3.01
//	1000    -3.28     -3.00   -3.01
//	5000    -4.14     -2.86   -3.01
//     15000    -4.95     -1.83   -3.01
//     20000    -4.73     -1.20   -3.01
//
// So 1-exp() is better than what was here and still 1.8dB out at the
// top, in the other direction.  Since every caller names a corner
// frequency and means the -3dB point by it, answer that question
// exactly instead.  The three inline copies model specific RC networks
// and can keep their own mapping; what they should not keep is being
// the only ones that are right.
//
// Written in terms of u = 1-cos(w) = 2*sin^2(w/2) rather than cos(w)
// directly, which matters more than it looks.  Substituting gives
//
//	alpha = sqrt(u*(u+2)) - u
//
// and that is the same expression with every cancellation taken out of
// it.  The direct form has to compute t*t-1 with t = 2-cos(w), and at
// 30Hz cos(w) is 0.9999923, so t*t-1 is 1.5e-5 built out of two floats
// that agree to seven digits - about two digits survive.  Coded that
// way it put a 20Hz filter's corner at 30.8Hz and a 30Hz one at 37.5Hz,
// which is worse at the bottom than the approximation it replaced was
// at the top, and worse exactly where frenchie.h's DC blocker and
// klon.h's coupling cap live.  Taking sin(w/2) instead keeps the small
// quantity small all the way through: sqrt(2u) dominates u rather than
// nearly cancelling it.
//
// Undefined above Nyquist, as it was before: fastsincos() wraps and the
// answer comes back down again.  Nothing asks.
//
static inline struct single_pole_coeff single_pole_freq(float freq)
{
	float s = fastsincos(0.5f * freq / SAMPLES_PER_SEC).sin;
	float u = 2.0f * s * s;
	struct single_pole_coeff coeff = { sqrtf(u * (u + 2.0f)) - u };
	return coeff;
}

//
// The same thing said in milliseconds, and it is exactly what
// time_constant() already computes - so ask it rather than write the
// exponential out a second time.  A zero or negative time is answered
// with alpha 1, which is "follow the input immediately"; see the note
// on time_constant() in util.h for why zero is reachable at all.
//
static inline struct single_pole_coeff single_pole_time(float ms)
{
	struct single_pole_coeff coeff = { 1.0f - time_constant(ms) };
	return coeff;
}

static inline struct single_pole_coeff single_pole_rc(float R, float C)
{
	return single_pole_time(R*C*1000);
}

static inline float single_pole_lpf(float in,
	struct single_pole_state *state,
	const struct single_pole_coeff coeff)
{
	return _single_pole_step(in, state, coeff);
}

static inline float single_pole_hpf_complementary(float in,
	struct single_pole_state *state,
	const struct single_pole_coeff coeff)
{
	return in - _single_pole_step(in, state, coeff);
}

// The proper way to do a single-pole high-pass filter,
// with 'R' being the filter pole location (1-alpha)
static inline float single_pole_hpf(float in,
	struct single_pole_state *state,
	const struct single_pole_coeff coeff)
{
	float R = 1.0f - coeff.alpha;
	float y = in + state->y;
	state->y = R * y - in;
	return y;
}
