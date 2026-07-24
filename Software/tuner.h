/*
 * Tuner UI with both a chromatic base version that tries to show
 * any dominant tone frequency, and a polyphonic display above it
 * that shows the individual string tunings.
 *
 * Note that the polyphonic mode shows a fixed string tuning target,
 * and right now that target tuning is fixed at the standard guitar
 * tuning (440Hz 'A'), but the code is set up to easily add other
 * tunings (but would then also need some setting UI to pick them).
 *
 * If you want microtonal tuning with non-standard scales for the
 * chromatic tuner, you're on your own, but it shouldn't be anything
 * horribly hard.
 */
#define MAX_STRINGS 8

struct tune_target {
	const char *name;
	float base_freq;
};

struct tuning {
	const char *name;
	int num_strings;
	const struct tune_target strings[MAX_STRINGS];
};

// The magnitude array re-uses the fft array to not be
// quite so piggy in memory use. But we might still have
// to shrink the FFT size at some point.
struct {
	union {
		complex_t fft[FFT_SIZE];
		float magnitudes[FFT_SIZE / 2];
	};
	float max_mag, avg_mag;

	// Chromatic tuning data
	float dominant_freq;
	float dominant_mag;

	// Polyphonic data
	float string_freq[MAX_STRINGS];
	float string_mag[MAX_STRINGS];
} tuner_state;

struct tune_result {
	int note_idx; // 0 means no result
	int cents;
	float mag;
};

struct tuner_results {
	int num_results;
	struct tune_result results[1 + MAX_STRINGS];
};

struct peak_info {
	float bin;
	float freq;
	float mag;
};

static inline bool get_peak(int i, struct peak_info *peak, float min_peak)
{
	float *center = tuner_state.magnitudes + i;
	float mag = *center;

	//
	// Note that the min_peak argument is only a quick
	// quick-and-dirty bin peak magnitude filter.
	//
	// In particular, it does not take into account that
	// the final magnitude of the peak is the combination
	// of the nearby bins and may be bigger than this first
	// simple filtering.
	//
	if (mag < min_peak)
		return false;

	//
	// Avoid noise: any peak we detect has to be
	// clearly above the noise floor, here fairly
	// arbitrarily defined to be "five times the
	// average magnitude and at least 5.0"
	//
	if (mag < 5.0f || mag < tuner_state.avg_mag * 5.0f)
		return false;

	//
	// We know the maximum magnitude we've seen.
	// Don't bother looking at anything smaller
	// than 5% of the max. It may be a local peak,
	// but it's still not interesting.
	//
	if (mag < tuner_state.max_mag * 0.05f)
		return false;

	//
	// It needs to be sharp enough to not only be at least
	// equal to its direct neighbors, but slightly bigger than
	// something two bins away
	//
	if (mag <= center[-1] || mag <= center[+1] ||
	    mag <= 1.2f * center[-2] || mag <= 1.2f * center[+2])
		return false;

	//
	// Ok, we have something that looks like a peak bin.
	//
	// Let's try to figure out what the actual exact
	// frequency would be that causes this bin pattern.
	//
	float y1 = center[-1];
	float y2 = mag;
	float y3 = center[+1];
	float p;

	// The audio analyzer uses a Hann window (see hanning() in analyze.h).
	// For a Hann window, the Grandke interpolation algorithm provides a
	// mathematically exact fractional bin offset. It calculates the offset
	// depending on which side of the peak is bigger, avoiding log() calls.
	if (y3 > y1) {
		p = (2.0f * y3 - y2) / (y3 + y2);
	} else {
		p = (y2 - 2.0f * y1) / (y1 + y2);
	}

	peak->bin = i + p;
	peak->freq = peak->bin * (12000.0f / FFT_SIZE);

	// A simple parabolic estimate is still fine for the magnitude, as we
	// mainly use it for relative peak comparisons and thresholding.
	peak->mag = y2 - 0.25f * (y1 - y3) * p;
	return true;
}

static inline void suppress_harmonics(void)
{
	int max_bin = FFT_SIZE / 2;
	for (int i = 2; i < max_bin / 2; i++) {
		struct peak_info peak;
		if (!get_peak(i, &peak, 0.0f))
			continue;

		for (int h = 2; h * peak.bin < max_bin; h++) {
			float target_exact = h * peak.bin;

			// Restrict suppression to the exact +/- 2.0 bin Hann window footprint
			int low = (int)floorf(target_exact - 2.0f);
			int high = (int)ceilf(target_exact + 2.0f);

			for (int j = low; j <= high; j++) {
				if (j >= max_bin) break;
				if (j < 0) continue;

				float d = fabsf((float)j - target_exact);
				float w = 0.0f;
				if (d <= 1.0f) {
					w = 1.0f - 0.5f * d * d;
				} else if (d <= 2.0f) {
					float x = 2.0f - d;
					w = 0.5f * x * x;
				}

				float suppression = peak.mag * w;
				if (tuner_state.magnitudes[j] > suppression)
					tuner_state.magnitudes[j] -= suppression;
				else
					tuner_state.magnitudes[j] = 0.0f;
			}
		}
	}
}

static inline void tuner_magnitudes(void)
{
	// Find the dominant fundamental for chromatic tuning
	float dominant_freq = 0.0f;
	float dominant_mag = 0.0f;

	// Search frequencies between ~15Hz and ~1.5kHz (E6 is 1318.5Hz)
	int min_bin = (int)(15.0f * FFT_SIZE / 12000.0f);
	int max_bin = (int)(1500.0f * FFT_SIZE / 12000.0f);
	if (min_bin < 2) min_bin = 2; // Need margin for i-2 check
	if (max_bin > FFT_SIZE / 2 - 3) max_bin = FFT_SIZE / 2 - 3; // Need margin for i+2 check

	for (int i = min_bin; i < max_bin; i++) {
		struct peak_info peak;

		if (!get_peak(i, &peak, dominant_mag))
			continue;

		//
		// We heavily prefer fundamentals over their harmonics,
		// so if we already have seen a dominant peak, discount
		// new peaks that are far away..
		//
		if (dominant_freq) {
			float distance = peak.freq / dominant_freq;
			if (peak.mag / distance < dominant_mag)
				continue;
		}

		dominant_freq = peak.freq;
		dominant_mag = peak.mag;
	}

	tuner_state.dominant_freq = dominant_freq;
	tuner_state.dominant_mag = dominant_mag;
}

static const struct tuning EADGBE = {
	.name = "Standard",
	.num_strings = 6,
	.strings = {
		{"E", 82.41f},  // E2
		{"A", 110.00f}, // A2
		{"D", 146.83f}, // D3
		{"G", 196.00f}, // G3
		{"B", 246.94f}, // B3
		{"E", 329.63f}, // E4
	},
};

static const struct tuning DADGAD = {
	.name = "DADGAD",
	.num_strings = 6,
	.strings = {
		{"D",  73.42f}, // D2
		{"A", 110.00f}, // A2
		{"D", 146.83f}, // D3
		{"G", 196.00f}, // G3
		{"A", 220.00f}, // A3
		{"D", 293.66f}, // D4
	},
};

// 6-string standard bass tuning
static const struct tuning BEADGC = {
	.name = "BEADGC",
	.num_strings = 6,
	.strings = {
		{"B",  30.87f}, // B0
		{"E",  41.20f}, // E1
		{"A",  55.00f}, // A1
		{"D",  73.42f}, // D2
		{"G",  98.00f}, // G2
		{"C", 130.81f}, // C3
	},
};

// 4-string standard bass tuning
static const struct tuning EADG = {
	.name = "EADG",
	.num_strings = 4,
	.strings = {
		{"E",  41.20f}, // E1
		{"A",  55.00f}, // A1
		{"D",  73.42f}, // D2
		{"G",  98.00f}, // G2
	},
};


static const struct tuning *const tunings[4] = {
	&EADGBE,
	&DADGAD,
	&BEADGC,
	&EADG,
};

static inline void find_string_peak(const struct tuning *current_tuning, int s)
{
	float target_freq = current_tuning->strings[s].base_freq;
	float target_bin = target_freq * ((float)FFT_SIZE / 12000.0f);

	// We restrict the search window to +/- 2.5% (about 43 cents).
	// This ensures that any peak we find is guaranteed to be closer to this
	// target note than to any adjacent note, allowing us to safely derive
	// the target note index directly from the measured frequency later.
	// It also neatly matches our UI, which visually clips the polyphonic
	// tuning arrows to a +/- 40 cent range anyway.
	int min_b = lrintf(target_bin * 0.975f);
	int max_b = lrintf(target_bin * 1.025f);
	if (min_b < 2) min_b = 2;

	float max_mag = 0.0f;
	int peak_b = 0;
	for (int i = min_b; i <= max_b; i++) {
		if (tuner_state.magnitudes[i] > max_mag) {
			max_mag = tuner_state.magnitudes[i];
			peak_b = i;
		}
	}

	//
	// Check that it's an actual peak.
	//
	// If get_peak() fails, it will leave the
	// (zeroed) 'peak' variable unchanged.
	//
	struct peak_info peak = { };
	get_peak(peak_b, &peak, 0.0f);

	tuner_state.string_freq[s] = peak.freq;
	tuner_state.string_mag[s] = peak.mag;
}

static inline void polyphonic_tuner_magnitudes(const struct tuning *current_tuning)
{
	for (int s = 0; s < current_tuning->num_strings; s++)
		find_string_peak(current_tuning, s);
}


// Helper to calculate the MIDI note number (69 = A4 440Hz) and cent deviation.
static inline struct tune_result calculate_note_and_cents(float freq)
{
	struct tune_result res = { 0, 0 };
	if (freq <= 0.0f)
		return res;

	float note_float = 69.0f + 12.0f * log2f(freq / 440.0f);
	res.note_idx = (int)(rintf(note_float));
	res.cents = (int)lrintf((note_float - res.note_idx) * 100.0f);
	return res;
}

static void compute_tuner_results(const struct tuning *current_tuning, struct tuner_results *out)
{
	out->num_results = 1 + current_tuning->num_strings;

	// Chromatic
	out->results[0] = calculate_note_and_cents(tuner_state.dominant_freq);
	out->results[0].mag = tuner_state.dominant_mag;

	// Polyphonic
	for (int s = 0; s < current_tuning->num_strings; s++) {
		if (tuner_state.string_mag[s] > 0.0f) {
			out->results[1 + s] = calculate_note_and_cents(tuner_state.string_freq[s]);
			out->results[1 + s].mag = tuner_state.string_mag[s];
		} else {
			out->results[1 + s].note_idx = 0;
			out->results[1 + s].cents = 0;
			out->results[1 + s].mag = 0.0f;
		}
	}
}

static const char *const note_names[12] = {
	"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static int prev_note_idx[1 + MAX_STRINGS] = {0};

static void send_tuner_midi(const struct tuner_results *results)
{
	for (int i = 0; i < results->num_results; i++) {
		int current_note = results->results[i].note_idx;
		int prev_note = prev_note_idx[i];
		int cents = results->results[i].cents;

		uint8_t ch = i; // Chromatic on ch 0, Strings on ch 1-8

		if (current_note != prev_note) {
			if (prev_note != 0) {
				send_midi_note_off(ch, prev_note, 0);
			}
			if (current_note != 0) {
				send_midi_note_on(ch, current_note, 100);
			}
			prev_note_idx[i] = current_note;
		}

		if (current_note != 0) {
			send_midi_pitch_bend(ch, cents * 41);

			float mag = results->results[i].mag;
			int vol = (int)(log2f(mag + 1.0f) * 10.5f);
			if (vol > 127) vol = 127;
			if (vol < 0) vol = 0;
			send_midi_channel_pressure(ch, vol);
		}
	}
}

static void tuner_mode_ui(void)
{
	unsigned int tuning_idx = settings.tuning;
	if (tuning_idx >= ARRAY_SIZE(tunings))
		tuning_idx = 0;
	const struct tuning *current_tuning = tunings[tuning_idx];

	unsigned int write_idx = analyzer.write_index;

	// Catch up if CPU0 falls too far behind CPU1
	if (write_idx - analyzer.read_index > ANALYZE_RING_SIZE - FFT_SIZE) {
		analyzer.read_index = write_idx - FFT_SIZE;
	}

	if (write_idx - analyzer.read_index < FFT_SIZE)
		return;

	// Copy data from ring buffer and apply Hann window
	for (int i = 0; i < FFT_SIZE; i++) {
		float sample = analyzer.ring_buf[(analyzer.read_index + i) & ANALYZE_RING_MASK];
		tuner_state.fft[i] = sample * hanning(i);
	}

	// Run FFT
	fft(tuner_state.fft, FFT_SHIFT);

	// Compute the magnitude of the bins
	//
	// NOTE! The fft[] and magnitudes[] arrays are a
	// union in the tuner_state structure. This only works
	// because we just walk the (bigger) fft 'complex_t'
	// forward and then store the resulting magnitude
	// result on top of old fft values.
	//
	float sum_mag = 0.0f;
	float max_mag = 0.0f;
	for (int i = 0; i < FFT_SIZE / 2; i++) {
		float mag = __builtin_cabsf(tuner_state.fft[i]);
		tuner_state.magnitudes[i] = mag;
		sum_mag += mag;
		if (mag > max_mag)
			max_mag = mag;
	}

	tuner_state.max_mag = max_mag;
	tuner_state.avg_mag = sum_mag / (FFT_SIZE / 2);

	suppress_harmonics();

	tuner_magnitudes();

	polyphonic_tuner_magnitudes(current_tuning);

	struct tuner_results results;
	compute_tuner_results(current_tuning, &results);

	send_tuner_midi(&results);

	// Overlap by advancing read_idx by a fraction of FFT_SIZE
	// FFT_SIZE / 16 = 512 samples. At 12kHz, this means 23 updates per second.
	analyzer.read_index += FFT_SIZE / 16;
}
