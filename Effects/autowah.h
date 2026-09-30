// NAME: Auto-Wah [AUTOWAH]
// PRIORITY: 35
// ABOUT: An envelope filter that sweeps a resonant filter based on pick attack.
// ABOUT: Dynamic funk quack and vocal wah sounds tracking your playing.
// POT: "Sens" LINEAR(0.0 36.0) = 18.0 dB
// INFO: Sensitivity to pick dynamics, as gain on the envelope. Turn up for softer
// INFO: playing or lower-output pickups; turn down if hard chords keep the filter
// INFO: open continuously.
// POT: "Freq" FREQUENCY(80.0 1200.0) = 250.0 Hz
// INFO: Resting frequency of the filter before envelope modulation, or in Down
// INFO: mode the frequency it sweeps down to.
// INFO: Lower frequencies produce deep vocal thumps; higher frequencies start
// INFO: closer to the nasal vocal range.
// POT: "Resonance" EXPONENTIAL(1.0 12.0) = 4.5
// INFO: Filter resonance (Q). High values produce a sharp, vocal "quack";
// INFO: low values give a subtle, smooth frequency sweep. The output is scaled
// INFO: to stay at roughly the same loudness across the range.
// POT: "Range" LINEAR(0.5 4.5) = 3.5 oct
// INFO: Maximum sweep range in octaves above the Freq setting. In Down mode the
// INFO: filter rests at the top of this range.
// POT: "Mode" ENUM(Lowpass Bandpass) = Lowpass
// INFO: Filter mode. Lowpass retains bottom end while sweeping harmonics;
// INFO: Bandpass isolates the sweeping resonant peak for classic crying wah.
// POT: "Direction" ENUM(Up Down) = Up
// INFO: Sweep direction. Up sweeps higher with harder pick attack (classic funk quack);
// INFO: Down sweeps downward from the peak (reverse envelope "oww" synth bass).
// POT: "Decay" LINEAR(20.0 500.0) = 140.0 ms
// INFO: How fast the filter drops back down to resting frequency once the note decays.
// POT: "Level" LINEAR(-12.0 12.0) = 4.0 dB
// INFO: Output level trim, in dB. Useful to match bypass volume across different
// INFO: resonance and pickup settings.
// DEFAULT_MIX: 1.0

// Auto-Wah envelope filter by Jacky Mpoka <jackympoka22@gmail.com>

static struct {
	float sens;
	float base_freq;
	float Q;
	float octaves;
	float q_gain;
	float level;
	int mode;
	int direction;

	struct envelope env;
	struct biquad bq;
} autowah;

static void autowah_init(unsigned char pot[10])
{
	autowah.sens = db_to_level(autowah_sens_pot(pot));
	autowah.base_freq = autowah_freq_pot(pot);
	autowah.Q = autowah_resonance_pot(pot);
	autowah.octaves = autowah_range_pot(pot);
	autowah.mode = (int)autowah_mode_pot(pot);
	autowah.direction = (int)autowah_direction_pot(pot);
	// Both filters peak at about Q times the input, so a resonant
	// setting is also a louder one: 12dB louder at Q 12 than at Q 1.
	// Dividing by sqrt(Q) brings that down to about 3dB.
	autowah.q_gain = 1.0f / sqrtf(autowah.Q);
	autowah.level = db_to_level(autowah_level_pot(pot));

	// Attack at 8ms to track fast pick transients without latency;
	// release tracks note decay via the Decay pot.
	envelope_init(&autowah.env, 8.0f, autowah_decay_pot(pot));
}

static float autowah_step(float in)
{
	float env = envelope_step(&autowah.env, in);

	// At the default +18dB, normal playing sweeps the whole range on
	// its peaks; the top of the pot does the same for a quiet pickup.
	float drive = env * autowah.sens;
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

	float filtered = biquad_step(&autowah.bq, in) * autowah.q_gain;

	return tanhf(filtered) * autowah.level;
}
