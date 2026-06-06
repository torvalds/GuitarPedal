//
// Simple single-pole helpers
//
// This uses trivial wrapper structures for type safety (and
// incidentally to look a bit like the biquad code).
//
// Note that you can precompute 'alpha' when it makes sense, but for
// things like constant frequencies or times, it's much better to just
// do them as part of the step function, since that will allow the
// compiler to just fold all the computations.

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

// Standard fast approximations when far away from Nyquist
static inline struct single_pole_coeff single_pole_freq(float freq)
{
	float omega = freq * (TWOPI / SAMPLES_PER_SEC);
	struct single_pole_coeff coeff = { omega / (1 + omega) };
	return coeff;
}

static inline struct single_pole_coeff single_pole_time(float ms)
{
	float tau = 1.0 / (SAMPLES_PER_MSEC * ms);
	struct single_pole_coeff coeff = { tau / (1 + tau) };
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
