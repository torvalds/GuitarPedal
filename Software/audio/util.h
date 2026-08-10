// Various utility functions mainly for
// imprecise but fast floating point

//
// Sized integer types I'm used to from the kernel.
//
// I dislike 'uint32_t' as being unwieldly (and historically not
// available in all environments, so you end up with a mess of
// configuration), and 'uint' as not having a well-defined size.
//
// I'm not using the 64-bit types yet, but the RP2354 has 32x32
// multiplies giving a 64-bit result, so I'm considering doing
// some fixed-point math, and this preps for it.
//

// Declare the fast functions with hardware support
float rintf(float);
float sqrtf(float);
float fabsf(float);
float floorf(float);
float ceilf(float);

//
// 'lrintf()' is *not* one of them.  Gcc has no scalar VFP lrint
// pattern for M-profile, so it never expands inline and instead
// turns into a call to the newlib software implementation - stack
// frame, magic-number rounding, bit extraction and all.
//
// Casting the result of 'rintf()' generates the two instructions we
// actually wanted ('vrintx.f32' + 'vcvt.s32.f32'), so just do that
// and keep the standard name.
//
static inline long int lrintf(float x)
{
	return (long int)rintf(x);
}

#define log10f(x) (log2f(x)/LOG2_10)

#define TWO_POW_32 (4294967296.0f)
#define LN2 0.69314718055994530942
#define LOG2_e (1/LN2)
#define LOG2_10 3.3219280948873623479
#define TWOPI 6.28318530718

#define SAMPLES_PER_MSEC (SAMPLES_PER_SEC * 0.001)

// Turn 0..120 pot to 0.0..1.0 float internally, and back again
#define POT_TO_FLOAT(pot) ((pot) / 120.0f)
#define FLOAT_TO_POT(f) lrintf((f) * 120.0f)

// Turn 0..1 into a range
#define linear(pot, a, b)	((a)+(pot)*((b)-(a)))
#define cubic(pot, a, b)	linear((pot)*(pot)*(pot), a, b)

// Turn a pot value into some reasonable range
#define linear_pot(pot, a, b)	linear(POT_TO_FLOAT(pot), a, b)
#define frequency_pot(pot, a, b) cubic(POT_TO_FLOAT(pot), a, b)

static inline float clamp(float x, float min, float max)
{
	return x < min ? min : (x > max ? max : x);
}

static inline float u32_to_fraction(u32 val)
{
	return (1.0/TWO_POW_32) * val;
}

static inline u32 fraction_to_u32(float val)
{
	return (u32) (val * TWO_POW_32);
}

static inline void __sample_array_write(float val, unsigned *idxp, unsigned mask, float *array)
{
	array[mask & ++*idxp] = val;
}

#define sample_array_write(val, idxp, array) __sample_array_write(val, idxp, ARRAY_SIZE(array)-1, array)

static inline float __sample_array_read(float delay, unsigned *idxp, unsigned mask, float *array)
{
	int i = (int) delay;
	float frac = delay - i;
	int idx = *idxp - i;

	float a = array[mask & idx];
	float b = array[mask & --idx];
	return linear(frac, a, b);
}

#define sample_array_read(d, idxp, array) __sample_array_read(d, idxp, ARRAY_SIZE(array)-1, array)

// int16_t delay-line variants: same ring-buffer logic, float<->s16 conversion at
// the boundary. 1/32767 keeps the range symmetric around zero.
static inline void __sample_array_write_s16(float val, unsigned *idxp, unsigned mask, int16_t *array)
{
	array[mask & ++*idxp] = (int16_t)(val * 32767.0f);
}

#define sample_array_write_s16(val, idxp, array) __sample_array_write_s16(val, idxp, ARRAY_SIZE(array)-1, array)

static inline float __sample_array_read_s16(float delay, unsigned *idxp, unsigned mask, int16_t *array)
{
	int i = (int) delay;
	float frac = delay - i;
	int idx = *idxp - i;

	float a = (float)array[mask & idx] * (1.0f / 32767.0f);
	float b = (float)array[mask & --idx] * (1.0f / 32767.0f);
	return linear(frac, a, b);
}

#define sample_array_read_s16(d, idxp, array) __sample_array_read_s16(d, idxp, ARRAY_SIZE(array)-1, array)

#include "log2.h"

#define LOG2_STEPS (1<< LOG2_STEP_SHIFT)

float __audio_func(log2f)(float x)
{
	union { float f; unsigned int i; } u = { x };

	//
	// Nothing here wants the logarithm of a negative number, and there
	// is no answer to give if it did.  The guard is not about the
	// caller being right, it is about being wrong *quietly*: the shift
	// below does not mask off the sign bit, so a negative argument used
	// to come back as a large finite positive - log2f(-1) was +256 -
	// and +256 handed to pow2() lands past its own top end.  Two
	// plausible-looking numbers make a plausible-looking answer.
	//
	// -127 is what zero already returned by accident, and it is the
	// right floor to keep: 2^-127 is under the smallest normal float,
	// and it composes safely, since pow2() of anything that negative
	// saturates to zero.
	//
	if (x <= 0.0f)
		return -127.0f;

	// Extract exponent and set it to zero (127)
	int exp = (u.i >> 23) - 127;
	u.i = 0x3f800000 | (u.i & 0x7fffff);
	x = u.f;

	// Lookup table index and fraction
	x = x*LOG2_STEPS - LOG2_STEPS;
	int idx = (int) x;
	x -= idx;

	return exp + linear(x, log2_table[idx], log2_table[idx+1]);
}

#include "pow2.h"

#define POW2_STEPS (1<< POW2_STEP_SHIFT)

float __audio_func(pow2)(float x)
{
	// Integer and fractional parts
	int exp = (int)floorf(x);
	x -= exp;

	//
	// Saturate at both ends rather than wrapping at one of them.
	//
	// The low guard was always here and is exact enough: 2^-31 is
	// already under anything this is asked for.  The high end used to
	// be a comment saying "we'll return random values, don't do it",
	// which was true - '1u << exp' shifts by exp & 31, so pow2(32) came
	// back as 1 and pow2(40) as 256, a tiny number for a huge one.
	//
	// Neither end is reachable from any pot today.  Both are one
	// comparison, which is cheaper than continuing to wonder, and this
	// is the function every dB in the pedal goes through.
	//
	// Saturating to 2^31 rather than to FLT_MAX on purpose: FLT_MAX
	// times almost anything is the infinity this is here to avoid, and
	// -ffast-math has already promised the compiler there aren't any.
	// 2^31 is the top of what this function claims to cover and has
	// room to be multiplied by.
	if (exp < -31)
		return 0.0;
	if (exp > 31)
		return 2147483648.0f;

	// Lookup table index and fraction
	x *= POW2_STEPS;
	int idx = (int) x;
	x -= idx;

	// Linear interpolation on table lookup
	x = linear(x, pow2_table[idx], pow2_table[idx+1]);

	if (exp >= 0)
		return x * (float)(1u << exp);
	return x / (float)(1u << -exp);
}

#define expf(x) pow2(LOG2_e*(x))

// We can calculate sin/cos at the same time using
// the table lookup. It's "GoodEnough(tm)" and with
// 256 entries it's good to about 5.3 digits of
// precision if I tested it right.
//
// Don't use this for real work. For audio? It's fine.
#include "quarter_sine.h"

#define QUARTER_SINE_STEPS (1<< QUARTER_SINE_STEP_SHIFT)

struct sincos { float sin, cos; };

// positive phase numbers only, please..
struct sincos __audio_func(fastsincos)(float phase)
{
	phase *= 4;
	int quadrant = (int)phase;
	phase -= quadrant;

	phase *= QUARTER_SINE_STEPS;
	int idx = (int) phase;
	phase -= idx;

	float a = quarter_sin[idx];
	float b = quarter_sin[idx+1];

	float x = a + (b-a)*phase;

	idx = QUARTER_SINE_STEPS - idx;
	a = quarter_sin[idx];
	b = quarter_sin[idx-1];

	float y = a + (b-a)*phase;

	if (quadrant & 1) {
		float tmp = -x; x = y; y = tmp;
	}
	if (quadrant & 2) {
		x = -x; y = -y;
	}

	return (struct sincos) { x, y };
}

// Half-time coefficient calculation:
//	= exp( -1 / (ms * SAMPLES_PER_MSEC) )
//
// Zero is answered directly rather than computed.  It is a reachable
// setting - [CHAIN]'s Attack is LINEAR(0.0 10.0), so the bottom of that
// pot is exactly zero milliseconds - and computing it divides by zero,
// which hands pow2() an infinity and leaves the result depending on
// which way the float-to-int conversion saturates.  ARM saturates
// toward the sign, so the pedal got INT_MIN, pow2()'s 'exp < -31' guard
// fired and the answer came out 0.0 anyway; x86 saturates the other
// way, misses the guard and indexes pow2_table[] with garbage.  Same
// source, same -ffast-math, one of them segfaults.
//
// So the value here was never in doubt - zero is what an instant attack
// wants, since linear(0, curr, prev) is curr - only whether we were
// entitled to it.  -ffast-math implies -ffinite-math-only, which is a
// promise that no infinity ever appears, and this was quietly breaking
// that promise on a value a MIDI CC can set.
//
// Negative is folded in with it.  Nothing generates one, and if
// something did the computed coefficient would be greater than 1 and
// the envelope would run away rather than follow anything.
static inline float time_constant(float ms)
{
	if (ms <= 0.0f)
		return 0.0f;
	return expf(-1 / SAMPLES_PER_MSEC / ms);
}

static inline float db_to_level(float db)
{
	return pow2(LOG2_10 / 20.0f * db);
}

//
// The same, for a biquad's 'A', which is the square root of the level -
// see biquad.h.  Half the constant rather than a sqrtf() of the answer,
// so asking for it costs exactly what db_to_level() costs, and the
// square root the biquads used to take goes away entirely.
//
static inline float db_to_A(float db)
{
	return pow2(LOG2_10 / 40.0f * db);
}

// [𝟓/4]-Padé approximant for tanh
static inline float tanhf(float x)
{
	float x2 = x*x;
	float n = x * (x2 * (x2 + 105) + 945);
	float d = x2 * (15 * x2 + 420) + 945;

	// Limit result to ±1 (d is always positive: even exponents)
	float abs_n = fabsf(n);
	if (d < abs_n) d = abs_n;

	return n / d;
}
