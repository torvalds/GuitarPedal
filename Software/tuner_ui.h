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

//
// This is very wasteful. The magnitude array could probably
// re-use the FFT array and be a union in the analyzer state?
//
struct {
	float magnitudes[FFT_SIZE / 2];
	float avg_mag;

	// Chromatic tuning data
	float dominant_freq;
	float dominant_mag;

	// Polyphonic data
	float string_freq[MAX_STRINGS];
	float string_mag[MAX_STRINGS];
	float string_target_freq[MAX_STRINGS];
} tuner_state;

static inline void tuner_magnitudes(float global_max_mag)
{
	// Find the dominant fundamental for chromatic tuning
	float dominant_freq = 0.0f;
	float dominant_mag = 0.0f;
	struct { float freq; float mag; } peaks[64];
	int num_peaks = 0;

	// Search frequencies between ~15Hz and ~1.5kHz (E6 is 1318.5Hz)
	int min_bin = (int)(15.0f * FFT_SIZE / 12000.0f);
	int max_bin = (int)(1500.0f * FFT_SIZE / 12000.0f);
	if (min_bin < 2) min_bin = 2; // Need margin for i-2 check
	if (max_bin > FFT_SIZE / 2 - 3) max_bin = FFT_SIZE / 2 - 3; // Need margin for i+2 check

	for (int i = min_bin; i < max_bin; i++) {
		float mag = tuner_state.magnitudes[i];
		if (mag > 1.0f) {
			if (mag > tuner_state.magnitudes[i-1] &&
			    mag > tuner_state.magnitudes[i+1] &&
			    mag > tuner_state.magnitudes[i-2] * 2.0f &&
			    mag > tuner_state.magnitudes[i+2] * 2.0f) {

				float y1 = tuner_state.magnitudes[i - 1];
				float y2 = mag;
				float y3 = tuner_state.magnitudes[i + 1];
				float p = 0.0f;
				float denom = y1 - 2.0f * y2 + y3;
				if (denom != 0.0f) {
					p = 0.5f * (y1 - y3) / denom;
				}

				float freq = (i + p) * (12000.0f / FFT_SIZE);
				if (num_peaks < 64) {
					peaks[num_peaks].freq = freq;
					peaks[num_peaks].mag = mag;
					num_peaks++;
				}
			}
		}
	}

	if (num_peaks > 0) {
		int best_idx = 0;
		for (int i = 1; i < num_peaks; i++) {
			if (peaks[i].mag > peaks[best_idx].mag) {
				best_idx = i;
			}
		}

		dominant_freq = peaks[best_idx].freq;
		dominant_mag = peaks[best_idx].mag;

		// Search downwards for a valid base (harmonic root)
		for (int i = 0; i < best_idx; i++) {
			float ratio = dominant_freq / peaks[i].freq;
			float closest_int = rintf(ratio);

			// Allow 3% deviation for harmonics
			if (fabsf(ratio - closest_int) < 0.03f * closest_int) {
				// Base must be at least 20% of the loudest peak's magnitude
				if (peaks[i].mag > dominant_mag * 0.20f) {
					dominant_freq = peaks[i].freq;
					break; // Found the lowest valid base
				}
			}
		}
	}

	tuner_state.dominant_freq = dominant_freq;
	tuner_state.dominant_mag = dominant_mag;
}

static const struct tuning std_tuning = {
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

static const struct tuning *current_tuning = &std_tuning;

static inline void polyphonic_tuner_magnitudes(float global_max_mag)
{
	for (int s = 0; s < current_tuning->num_strings; s++) {
		float best_mag = 0.0f;
		float best_freq = 0.0f;

		for (int t = 0; t < 3; t++) {
			float target_freq = current_tuning->strings[s].base_freq * (t == 0 ? 0.5f : (t == 1 ? 1.0f : 2.0f));
			float target_bin = target_freq * ((float)FFT_SIZE / 12000.0f);
			int min_b = (int)(target_bin * 0.965f + 0.5f); // +/- 3.5% (about 60 cents)
			int max_b = (int)(target_bin * 1.035f + 0.5f);
			if (min_b < 2) min_b = 2;

			float max_mag = 0.0f;
			int peak_b = 0;
			for (int i = min_b; i <= max_b; i++) {
				if (tuner_state.magnitudes[i] > max_mag) {
					max_mag = tuner_state.magnitudes[i];
					peak_b = i;
				}
			}

			// Require the peak to be at least 5% of the global max to be considered a fundamental
			if (max_mag > tuner_state.avg_mag * 5.0f && max_mag > 1.0f && max_mag > global_max_mag * 0.05f) {
				// Is it a true local peak, and is it tight?
				// We check +/- 2 bins to avoid rejecting pure tones that fall exactly between two bins,
				// while still rejecting wide frequency smearing from string attacks.
				if (tuner_state.magnitudes[peak_b] > tuner_state.magnitudes[peak_b-1] &&
				    tuner_state.magnitudes[peak_b] > tuner_state.magnitudes[peak_b+1] &&
				    tuner_state.magnitudes[peak_b] > tuner_state.magnitudes[peak_b-2] * 2.0f &&
				    tuner_state.magnitudes[peak_b] > tuner_state.magnitudes[peak_b+2] * 2.0f) {

					float y1 = tuner_state.magnitudes[peak_b - 1];
					float y2 = tuner_state.magnitudes[peak_b];
					float y3 = tuner_state.magnitudes[peak_b + 1];
					float p = 0.0f;
					float denom = y1 - 2.0f * y2 + y3;
					if (denom != 0.0f) {
						p = 0.5f * (y1 - y3) / denom;
					}
					float freq = (peak_b + p) * (12000.0f / FFT_SIZE);

					// Always lock the frequency to the lowest valid target to anchor the 2-octave limit
					if (best_freq == 0.0f) {
						best_freq = freq;
						tuner_state.string_target_freq[s] = target_freq;
					}
					// But adopt the magnitude of the strongest harmonic in the series
					if (max_mag > best_mag) {
						best_mag = max_mag;
					}
				}
			}
		}

		tuner_state.string_freq[s] = best_freq;
		tuner_state.string_mag[s] = best_mag;

		// Final column suppression
		if (best_mag < global_max_mag * 0.20f) {
			tuner_state.string_mag[s] = 0.0f;
		}
	}

	// User heuristic: find the lowest active frequency and never walk up more than 2 octaves.
	float lowest_freq = 9999.0f;
	for (int s = 0; s < current_tuning->num_strings; s++) {
		if (tuner_state.string_mag[s] > 0.0f && tuner_state.string_freq[s] < lowest_freq) {
			lowest_freq = tuner_state.string_freq[s];
		}
	}

	if (lowest_freq < 9999.0f) {
		float max_allowed_freq = lowest_freq * 4.05f; // 2 octaves + tiny margin
		for (int s = 0; s < current_tuning->num_strings; s++) {
			if (tuner_state.string_freq[s] > max_allowed_freq) {
				tuner_state.string_mag[s] = 0.0f;
			}
		}
	}

	// Harmonic suppression (currently only applied to standard 6-string)
	if (current_tuning->num_strings == 6) {
		// Low E harmonics: B3 (3x) and E4 (4x)
		if (tuner_state.string_mag[0] > 0.0f) {
			if (tuner_state.string_mag[4] < tuner_state.string_mag[0] * 0.25f)
				tuner_state.string_mag[4] = 0.0f;
			if (tuner_state.string_mag[5] < tuner_state.string_mag[0] * 0.25f)
				tuner_state.string_mag[5] = 0.0f;
		}
		// A harmonics: E4 (3x)
		if (tuner_state.string_mag[1] > 0.0f) {
			if (tuner_state.string_mag[5] < tuner_state.string_mag[1] * 0.25f)
				tuner_state.string_mag[5] = 0.0f;
		}
	}
}


// Forward declare to_ascii from ui.h
static char *to_ascii(unsigned char term, uint32_t val, char *p, int digits, int decimals);
static char *float_to_ascii(float val, int places);

static void draw_chromatic(void)
{
	float freq = tuner_state.dominant_freq;
	float note_float = 69.0f + 12.0f * log2f(freq / 440.0f);
	int note_idx = (int)(rintf(note_float));
	float cents = (note_float - note_idx) * 100.0f;

	static const char *const note_names[12] = {
		"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
	};

	int name_idx = note_idx % 12;
	if (name_idx < 0) name_idx += 12;

	const char *name = note_names[name_idx];
	int octave = (note_idx / 12) - 1;

	// Build a string like "C#4"
	char full_name[8];
	int name_len = 0;
	while (name[name_len]) {
		full_name[name_len] = name[name_len];
		name_len++;
	}
	full_name[name_len++] = '0' + octave;
	full_name[name_len] = '\0';

	char cents_str[8];
	char *end = cents_str + 8;
	end = to_ascii('\0', abs((int)cents), end, 1, 0);
	if (cents <= -0.5f) *--end = '-';
	else if (cents >= 0.5f) *--end = '+';

	sh1106_puts_8x16(64 - name_len * 4, 46, full_name);

	int cents_len = strlen(end);
	sh1106_puts_6x8(64 - cents_len * 3, 66, end);

	// Display exact frequency to the right of the chromatic section
	sh1106_puts_6x8(126 - 5 * 6, 50, float_to_ascii(freq, 4));

	// Huge needle spanning most of screen
	int bar_x = 64 + (int)cents; // 50 cents = 50 pixels -> 14 to 114
	if (bar_x < 14) bar_x = 14;
	if (bar_x > 114) bar_x = 114;

	sh1106_hline(14, 104, 100); // Axis line
	sh1106_vline(64, 94, 21); // Center tick
	sh1106_rectangle(bar_x - 2, 84, 5, 41, rect_filled); // Needle
}

static void draw_analyzer(void)
{
	if (analyzer.index < FFT_SIZE)
		return;

	// Run FFT
	fft(analyzer.buf, FFT_SHIFT);

	// Compute the magnitude of the bins
	float sum_mag = 0.0f;
	float global_max_mag = 0.0f;
	for (int i = 0; i < FFT_SIZE / 2; i++) {
		float mag = __builtin_cabsf(analyzer.buf[i]);
		tuner_state.magnitudes[i] = mag;
		sum_mag += mag;
		if (mag > global_max_mag)
			global_max_mag = mag;
	}

	tuner_state.avg_mag = sum_mag / (FFT_SIZE / 2);

	tuner_magnitudes(global_max_mag);

	sh1106_clear(0, 0, 128, 128);

	if (tuner_state.dominant_freq > 0.0f)
		draw_chromatic();

	polyphonic_tuner_magnitudes(global_max_mag);

	// Clear top background for polyphonic tuning display
	sh1106_clear(0, 0, 128, 36);

	int col_w = 128 / current_tuning->num_strings;
	int base_x = (128 - (current_tuning->num_strings * col_w)) / 2;

	for (int s = 0; s < current_tuning->num_strings; s++) {
			int s_x = base_x + s * col_w;

			if (tuner_state.string_mag[s] > 0.0f) {
				// Center the 8x16 char in the column
				sh1106_puts_8x16(s_x + (col_w - 8) / 2, 0, current_tuning->strings[s].name);

				float target_freq = tuner_state.string_target_freq[s];

				// Cents difference
				float note_float = 12.0f * log2f(tuner_state.string_freq[s] / target_freq);
				float cents = note_float * 100.0f;

				// Draw horizontal tuning needle
				int bar_w = col_w - 2; // leave 1px margin
				if (bar_w > 21) bar_w = 21; // cap width at 21px
				int x_offset = s_x + (col_w - bar_w) / 2;

				int bar_x = x_offset + (bar_w / 2) + (int)(cents * 0.15f);
				if (bar_x < x_offset) bar_x = x_offset;
				if (bar_x > x_offset + bar_w - 1) bar_x = x_offset + bar_w - 1;

				sh1106_hline(x_offset, 22, bar_w); // Axis line
				sh1106_vline(x_offset + (bar_w / 2), 20, 5); // Center tick
				sh1106_vline(bar_x, 18, 9); // Needle

				// Print numerical cents value below graph (y=26)
				char cents_str[8];
				char *end = cents_str + 8;
				end = to_ascii('\0', abs((int)cents), end, 1, 0);
				if (cents <= -0.5f) *--end = '-';
				else if (cents >= 0.5f) *--end = '+';

				int len = strlen(end);
				int text_x = s_x + (col_w - len * 6) / 2;
				sh1106_puts_6x8(text_x, 26, end);
			} else {
				// Inactive string: center 6x8 char
				sh1106_puts_6x8(s_x + (col_w - 6) / 2, 4, current_tuning->strings[s].name);
			}
		}

	analyzer.index = 0; // consumed

	sh1106_draw();
}
