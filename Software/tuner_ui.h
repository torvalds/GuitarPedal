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
} tuner_state;

static inline void tuner_magnitudes(float global_max_mag)
{
	// Find the dominant fundamental for chromatic tuning
	float dominant_freq = 0.0f;
	float dominant_mag = 0.0f;
	struct { float freq; float mag; } peaks[64];
	int num_peaks = 0;

	for (int i = 5; i < 450; i++) {
		float mag = tuner_state.magnitudes[i];
		if (mag > 3.0f) {
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

				float freq = (i + p) * (48000.0f / FFT_SIZE);
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
				// Base must be at least 2% of the loudest peak's magnitude
				if (peaks[i].mag > dominant_mag * 0.02f) {
					dominant_freq = peaks[i].freq;
					break; // Found the lowest valid base
				}
			}
		}
	}

	tuner_state.dominant_freq = dominant_freq;
	tuner_state.dominant_mag = dominant_mag;
}

// FFT_SIZE maps to 48kHz (and we only have magnitude bins for
// half of that). But for guitar tuning, we're only really
// interested in one octave up from the high E, which is 660Hz.
//
// So let's go to FFT_SIZE / 64, which maps to 750Hz
// (That's really just two bins per pixel - FFT_SHIFT is 14)
#define BINS_PER_PIXEL (FFT_SIZE/64/128)

static int analyzer_graph_fn(int x, void *arg)
{
	int bin = x*BINS_PER_PIXEL;
	float mag = 1.0;

	for (int i = 0; i < BINS_PER_PIXEL; i++)
		mag *= tuner_state.magnitudes[bin+i];

	// Logarithmic scaling for magnitude.
	// Scaled down to de-emphasize and fit in ~40 pixels at the bottom.
	int h = (int)rintf(3 * log2f(mag + 1.0f));
	if (h > 41) h = 41;
	if (h < 0) h = 0;

	return 127 - h; // 127 is bottom of screen
}

// Forward declare to_ascii from ui.h
static char *to_ascii(unsigned char term, uint32_t val, char *p, int digits, int decimals);

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

	// Clear middle area
	sh1106_clear(0, 40, 128, 44);

	sh1106_puts_8x16(64 - name_len * 4, 46, full_name);

	int cents_len = strlen(end);
	sh1106_puts_6x8(64 - cents_len * 3, 66, end);

	// Huge needle spanning most of screen
	int bar_x = 64 + (int)(cents * 0.5f); // 50 cents = 25 pixels -> 39 to 89
	if (bar_x < 14) bar_x = 14;
	if (bar_x > 114) bar_x = 114;

	sh1106_hline(14, 78, 100); // Axis line
	sh1106_vline(64, 76, 5); // Center tick
	sh1106_vline(bar_x, 74, 9); // Needle
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

	// Draw graph smaller at the bottom
	sh1106_graph(0, 127, 86, 127, analyzer_graph_fn, NULL);

	if (tuner_state.dominant_freq > 0.0f)
		draw_chromatic();

	analyzer.index = 0; // consumed

	sh1106_draw();
}
