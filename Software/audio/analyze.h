#include <stdatomic.h>

#define FFT_SHIFT 13
#define FFT_SIZE (1 << FFT_SHIFT)

#define ANALYZE_RING_SHIFT 14
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
	_Atomic unsigned int write_index;
	_Atomic unsigned int read_index;
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

// 4x downsampled data into continuous lock-free ring buffer
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

	unsigned int idx = atomic_load_explicit(&analyzer.write_index,
		memory_order_relaxed);
	unsigned int read_idx = atomic_load_explicit(&analyzer.read_index,
		memory_order_acquire);

	// The analyzer is optional, so drop data instead of overwriting a
	// window while the UI core is processing it.
	if (idx - read_idx >= ANALYZE_RING_SIZE)
		return;

	analyzer.ring_buf[idx & ANALYZE_RING_MASK] = sample;
	atomic_store_explicit(&analyzer.write_index, idx + 1,
		memory_order_release);
}
