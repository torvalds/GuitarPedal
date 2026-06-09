#define FFT_SHIFT 13
#define FFT_SIZE (1 << FFT_SHIFT)

//
// NOTE! This is accessed from both cores, but the
// logic is that the audio core only touches it as
// long as 'index' is less that the full buffer size,
// and the UI core only touches it once we have a
// full buffer.
//
// The audio core essentially hands it to the UI core
// when it increments the index to FFT_SIZE, and then
// the UI core then hands it back by simply setting
// index to zero when it is done.
//
struct analyze_state {
	complex_t buf[FFT_SIZE];
	unsigned int index;
	unsigned int seen_significant;
} analyzer;

// Hann function using the quarter_sine table. We don't
// do the standard "(1-cos(x))/2", we do "sin^2(x/2)"
// instead, and only use half the sine cycle.
//
// Half a sine cycle is the same as walking the quarter
// cycle forward and then backward.
static inline float hanning(unsigned int idx)
{
	const int fractional_bits = FFT_SHIFT - QUARTER_SINE_STEP_SHIFT -1;

	float frac = u32_to_fraction(idx << (32 - fractional_bits));
	idx >>= fractional_bits;

	unsigned int next = idx+1;
	if (idx >= QUARTER_SINE_STEPS) {
		idx = QUARTER_SINE_STEPS*2 - idx;
		next = idx-1;
	}

	float sin = linear(frac, quarter_sin[idx], quarter_sin[next]);
	return sin*sin;
}

// 4x downsampled data with a hanning window until we've filled the buffer
static inline void analyze_process_sample(float sample)
{
	// Downsample by 4x
	static float sample_sum;
	static int count;

	sample += sample_sum;
	if (3 & ++count) {
		sample_sum = sample;
		return;
	}

	sample_sum = 0;

	// We could do 'sample *= 0.25' here to correct
	// for adding up four samples, but we don't actually
	// care about the absolute values and will just do
	// an FFT on it to figure out the frequencies, so ...

	unsigned int idx = analyzer.index;
	if (idx >= FFT_SIZE)
		return;

	if (!idx)
		analyzer.seen_significant = 0;

	// Random: start from beginning if this is the
	// first significant sample we've seen. Let's not
	// pointlessly do the FFT on some buffer that
	// starts out almost entirely silent.
	if (sample > 0.04 && !analyzer.seen_significant) {
		idx = 0;
		analyzer.seen_significant = 1;
	}

	analyzer.buf[idx] = sample * hanning(idx);
	analyzer.index = ++idx;
}
