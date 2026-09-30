// NAME: Auto-Wah [AUTOWAH]
// PRIORITY: 35
// ABOUT: An envelope filter that sweeps a resonant filter based on pick attack.
// ABOUT: Dynamic funk quack and vocal wah sounds tracking your playing.
// POT: "Sens" LINEAR(0.0 1.0) = 0.50
// INFO: Sensitivity to pick dynamics. Turn up for softer playing or lower-output
// INFO: pickups; turn down if hard chords keep the filter open continuously.
// POT: "Freq" FREQUENCY(80.0 1200.0) = 250.0 Hz
// INFO: Resting frequency of the filter before envelope modulation.
// INFO: Lower frequencies produce deep vocal thumps; higher frequencies start
// INFO: closer to the nasal vocal range.
// POT: "Resonance" LINEAR(1.0 12.0) = 4.5
// INFO: Filter resonance (Q). High values produce a sharp, vocal "quack";
// INFO: low values give a subtle, smooth frequency sweep.
// POT: "Range" LINEAR(0.5 4.5) = 2.5 oct
// INFO: Maximum sweep range in octaves above (or below) the resting frequency.
// POT: "Mode" ENUM(Lowpass Bandpass) = 0
// INFO: Filter mode. Lowpass retains bottom end while sweeping harmonics;
// INFO: Bandpass isolates the sweeping resonant peak for classic crying wah.
// POT: "Direction" ENUM(Up Down) = 0
// INFO: Sweep direction. Up sweeps higher with harder pick attack (classic funk quack);
// INFO: Down sweeps downward from the peak (reverse envelope "oww" synth bass).
// POT: "Decay" LINEAR(20.0 500.0) = 140.0 ms
// INFO: How fast the filter drops back down to resting frequency once the note decays.
// POT: "Level" LINEAR(-12.0 12.0) = 0.0 dB
// INFO: Output level trim, in dB. Useful to match bypass volume across different
// INFO: resonance and pickup settings.
// DEFAULT_MIX: 1.0

// Auto-Wah envelope filter by Jacky Mpoka <jackympoka22@gmail.com>

static struct {
	float sens;
	float base_freq;
	float Q;
	float octaves;
	float level;
	int mode;
	int direction;

	struct envelope env;
	struct biquad bq;
} autowah;

static void autowah_init(unsigned char pot[10])
{
	autowah.sens = autowah_sens_pot(pot);
	autowah.base_freq = autowah_freq_pot(pot);
	autowah.Q = autowah_resonance_pot(pot);
	autowah.octaves = autowah_range_pot(pot);
	autowah.mode = (int)autowah_mode_pot(pot);
	autowah.direction = (int)autowah_direction_pot(pot);
	autowah.level = db_to_level(autowah_level_pot(pot));

	// Attack at 8ms to track fast pick transients without latency;
	// release tracks note decay via the Decay pot.
	envelope_init(&autowah.env, 8.0f, autowah_decay_pot(pot));
}

static float autowah_step(float in)
{
	float env = envelope_step(&autowah.env, in);

	// Scale envelope by sensitivity. Normal guitar signals hover around 0.1 - 0.5 peak.
	// A multiplier of 3.0 gives a good dynamic sweep across typical pickup levels.
	float drive = env * autowah.sens * 3.0f;
	if (drive > 1.0f)
		drive = 1.0f;

	if (drive > 0.05f)
		autowah_effect.intense = 1;

	// Calculate swept filter cutoff/center frequency in octaves
	float oct = drive * autowah.octaves;
	float f;
	if (autowah.direction == 0) {
		f = autowah.base_freq * pow2(oct);
	} else {
		float top_freq = autowah.base_freq * pow2(autowah.octaves);
		f = top_freq * pow2(-oct);
	}

	// Clamp to safe filter range (well below Nyquist)
	f = clamp(f, 40.0f, 16000.0f);

	if (autowah.mode == 0)
		_biquad_lpf(&autowah.bq.coeff, f, autowah.Q);
	else
		_biquad_bpf_peak(&autowah.bq.coeff, f, autowah.Q);

	float filtered = biquad_step(&autowah.bq, in);

	return tanhf(filtered) * autowah.level;
}
