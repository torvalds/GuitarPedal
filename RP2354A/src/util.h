// Various utility functions mainly for
// imprecise but fast floating point

#define TWO_POW_32 (4294967296.0f)
#define LN2 0.69314718055994530942

// pow2()-1 using taylor series expansion
// good enough for the range we're interested in
// (mainly 0 .. 1, but maybe somebody wants -1 .. 1)
static inline float fastpow2_m1(float x)
{
	const float c1 = LN2,
		    c2 = LN2*LN2/2,
		    c3 = LN2*LN2*LN2/6;
	float x2 = x*x;
	float x3 = x2*x;
	return c1*x + c2*x2 + c3*x3;
}

static inline float fastpow(float a, float b)
{
	union { float f; int i; } u = { a };
	u.i = (int) (b * (u.i - 1072632447) + 1072632447.0f);
	return u.f;
}

// Very random - but quick - function to smoothly
// limit x to -1 .. 1 when it approaches -2..2
//
// So you can add two values in the -1..1 range and
// then limit the sum to that range too.
static float limit_value(float x)
{
	float x2 = x*x;
	float x4 = x2*x2;
	return x*(1 - 0.19*x2 + 0.0162*x4);
}

static inline float uint_to_fraction(uint val)
{
	return (1.0/TWO_POW_32) * val;
}

static inline uint fraction_to_uint(float val)
{
	return (uint) (val * TWO_POW_32);
}

// Max ~1.25s delays at ~52kHz
#define SAMPLE_ARRAY_SIZE 65536
#define SAMPLE_ARRAY_MASK (SAMPLE_ARRAY_SIZE-1)
extern float sample_array[SAMPLE_ARRAY_SIZE];
extern int sample_array_index;

static inline void sample_array_write(float val)
{
	uint idx = SAMPLE_ARRAY_MASK & ++sample_array_index;
	sample_array[idx] = val;
}

static inline float sample_array_read(float delay)
{
	int i = (int) delay;
	float frac = delay - i;
	int idx = sample_array_index - i;

	float a = sample_array[SAMPLE_ARRAY_MASK & idx];
	float b = sample_array[SAMPLE_ARRAY_MASK & ++idx];
	return a + (b-a)*frac;
}
