//
// The analyzer: a copy of the left input, low-passed and decimated to
// 6 kHz, for core 0 to take FFTs of.  The output is muted while it is in
// use, and the mode says what is using it.
//
// 4096 points at 6 kHz is 1.46 Hz bins in a 0.68 s window.  The tuner
// interpolates between bins, so what the size sets for it is the
// window, which is how quickly a reading follows a tuning peg.
//
enum analyzer_mode {
	ANALYZE_OFF,
	ANALYZE_TUNER,
};

// Written by core 0 only
static enum analyzer_mode analyzer_mode;

#define ANALYZE_DECIMATE 8
#define ANALYZE_RATE (SAMPLES_PER_SEC / ANALYZE_DECIMATE)

#define FFT_SHIFT 12
#define FFT_SIZE (1 << FFT_SHIFT)
#define ANALYZE_BIN_HZ (ANALYZE_RATE / FFT_SIZE)

#define ANALYZE_RING_SHIFT 13
#define ANALYZE_RING_SIZE (1 << ANALYZE_RING_SHIFT)
#define ANALYZE_RING_MASK (ANALYZE_RING_SIZE - 1)

//
// NOTE! This is accessed from both cores, but the
// logic is that the audio core only writes to 'ring_buf'
// and increments 'write_index'. The UI core independently
// reads from the ring buffer and maintains the read index.
//
struct analyze_state {
	float ring_buf[ANALYZE_RING_SIZE];
	unsigned int write_index;
	unsigned int phase;		// core 1 only
	struct biquad lp[2];		// coefficients set before core 1 starts

	// Core 0 only
	unsigned int read_index;
	union {
		complex_t fft[FFT_SIZE];
		float magnitudes[FFT_SIZE / 2];
	};
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

// Fourth-order Butterworth at 2.4 kHz, ahead of the decimation to 6 kHz
static void analyzer_init(void)
{
	biquad_lpf(&analyzer.lp[0], 2400.0f, 0.5412f);
	biquad_lpf(&analyzer.lp[1], 2400.0f, 1.3066f);
}

// Decimated data into continuous lock-free ring buffer
static inline void analyze_process_sample(sample_t sample)
{
	float x = biquad_step(&analyzer.lp[0], sample.left);
	x = biquad_step(&analyzer.lp[1], x);
	if (++analyzer.phase < ANALYZE_DECIMATE)
		return;
	analyzer.phase = 0;

	unsigned int idx = analyzer.write_index;

	analyzer.ring_buf[idx & ANALYZE_RING_MASK] = x;
	smp_store_release(&analyzer.write_index, idx + 1);
}

// Start reading from what comes in next, not from what a previous mode left
static void analyzer_skip(void)
{
	analyzer.read_index = smp_load_acquire(&analyzer.write_index);
}

static void analyzer_set_mode(enum analyzer_mode mode)
{
	analyzer_skip();
	analyzer_mode = mode;
}

//
// Window and transform the next FFT_SIZE samples into analyzer.fft,
// then move on by 'hop' samples.  False when they are not all in yet.
//
static bool analyze_next_block(unsigned int hop)
{
	unsigned int write_idx = smp_load_acquire(&analyzer.write_index);

	// Catch up if core 0 falls too far behind core 1
	if (write_idx - analyzer.read_index > ANALYZE_RING_SIZE - FFT_SIZE)
		analyzer.read_index = write_idx - FFT_SIZE;

	if (write_idx - analyzer.read_index < FFT_SIZE)
		return false;

	for (int i = 0; i < FFT_SIZE; i++) {
		float sample = analyzer.ring_buf[(analyzer.read_index + i) & ANALYZE_RING_MASK];
		analyzer.fft[i] = sample * hanning(i);
	}
	fft(analyzer.fft, FFT_SHIFT);

	analyzer.read_index += hop;
	return true;
}
