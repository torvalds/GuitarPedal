//
// This is very wasteful. The magnitude array could probably
// re-use the FFT array and be a union in the analyzer state?
//
struct {
	float magnitudes[FFT_SIZE / 2];
	float avg_mag;
} tuner_state;

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

	// Logarithmic scaling for magnitude, standard
	// mapping is 20 * log10f(mag + 1.0f), and a
	// magnitude of 100 will map to ~40.
	//
	// But we use log2() which effectively added
	// a factor of 3.3, and we've multiplied the
	// magnitudes of two bins together, which gives
	// another extra factor of two.
	//
	// So the '20' turns into just 3, but then we,
	// want to expand the 40 to the 128 pixel high
	// display so we add another factor of 3, and
	// thus multiply the log2() by 9 instead.
	int h = (int)rintf(9* log2f(mag + 1.0f));
	if (h > 127) h = 127;
	if (h < 0) h = 0;

	return 127 - h; // 127 is bottom of screen
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

	sh1106_clear(0, 0, 128, 128);

	// Draw graph full screen
	sh1106_graph(0, 127, 0, 127, analyzer_graph_fn, NULL);
	analyzer.index = 0; // consumed

	sh1106_draw();
}
