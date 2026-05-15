//
// EchoKing for Cleveland Music Co. Hothouse DIY DSP Platform
// Converted to C for the RP2350 direct pedal format
//
// A digital model of the Maestro Echoplex family of tape delay units.
// The EP-1 and EP-2 used tube preamps (12AX7); the EP-3 switched to solid state.
//
// Three things define the sound: the preamp (always in the signal path, coloring
// the dry tone), the tape medium (bandwidth limits, soft saturation, and repeats
// that darken each pass), and the transport mechanics (wow, flutter, and a
// characteristic pitch glide when delay time changes). All three are modeled here.
//
// This is not a licensed product of or affiliated with Gibson Brands, Maestro, or
// any other trademark holder. It is an educational DSP example.
//

static struct effect echo_effect;

// LCG PRNG for tape noise
static uint32_t echo_noise_state = 1;
static inline float echo_get_white_noise(void)
{
	echo_noise_state = echo_noise_state * 1664525 + 1013904223;
	// return float from -1.0 to 1.0
	return (float)echo_noise_state / 2147483648.0f - 1.0f;
}

struct echo_model {
	float preamp_drive;
	float preamp_asymmetry;
	float record_fc;
	float playback_fc;
	float feedback_fc;
	float wow_scale;
	float wow_rate;
	float flutter_scale;
	float flutter_rate;
	float noise_floor;
	float max_feedback;
	float delay_smooth;
	int sos_capable;
};

static const struct echo_model echo_models[] = {
	// EP-1: Tube (12AX7 single-ended), early cartridge transport. Loosest
	// mechanics, strongest even-harmonic coloration, most wow and flutter.
	// The "vintage instability" model. No SOS -- that came later.
	{ 1.4f, 0.22f, 5500.0f, 4500.0f, 3500.0f, 1.40f, 1.2f, 1.20f, 8.0f,  0.0006f, 0.95f, 0.00006f, 0 },
	// EP-2: Tube (12AX7 + 12AU7), revised circuit. The canonical Echoplex sound.
	// Thick, warm, the dry tone is perceptibly colored even without any echo.
	// Sound-on-sound was available on later EP-2 units.
	{ 1.5f, 0.18f, 6000.0f, 5000.0f, 4000.0f, 1.00f, 1.3f, 1.00f, 9.0f,  0.0004f, 1.00f, 0.00008f, 1 },
	// EP-3: Solid state (JFET/FET), mid-1970s. Tighter, brighter, more focused.
	// The "EP-3 preamp boost" is famous as a standalone tone shaper.
	// Better motor regulation means much less wow and flutter. Explicit SOS feature.
	{ 1.2f, 0.04f, 7500.0f, 6000.0f, 5000.0f, 0.65f, 1.5f, 0.60f, 10.5f, 0.0002f, 1.05f, 0.00012f, 1 }
};

struct {
	float target_blend, blend;
	float target_sustain, sustain;
	float target_delay_s, delay_s;
	float target_record_level, record_level;
	float target_tone, tone;
	float target_wow, wow;

	int model_idx;
	const struct echo_model *model;
	float delay_min_s;
	float delay_max_s;
	int sos_mode;
	int preamp_only;

	u32 wow_phase;
	u32 wow_mod_phase;
	u32 flutter_phase;

	struct biquad record_lpf;
	struct biquad playback_lpf;
	struct biquad feedback_lpf;
	struct biquad noise_lpf;

	unsigned idx;
	float array[65536];
} echo;

static float echo_blend(signed char pot) { return linear_pot(pot, 0, 1); }
static float echo_sustain(signed char pot) { return linear_pot(pot, 0, 1.1f); }
static float echo_time_pot(signed char pot) { return POT_TO_FLOAT(pot); } // 0 to 1
static float echo_record(signed char pot) { return linear_pot(pot, 0.1f, 2.0f); }
static float echo_tone_pot(signed char pot) { return linear_pot(pot, 0, 1); }
static float echo_wow(signed char pot) { return linear_pot(pot, 0, 1); }

//
// The time and tone values depend on multiple pots, and use the current
// echo state to display a more human-friendly value.
//
// In order to do that, we need to look not just at the current pot
// value, but the whole 'current set of pots'. Thus the whole games
// with the current sequence number etc.
//
// Note that this duplicates the logic at ->init() time, but that is
// done on the audio processing core asynchronously. So this could
// maybe use some common helper for cleanups, but not shared state.
//
// Note: This is a semantic new feature added during conversion to display
// real-world units (ms, Hz) on the UI screen based on live pot positions.
//
static float echo_time_display(signed char pot)
{
	const signed char *pots = echo_effect.pot_values[echo_effect.seq & 1];
	float time_val = POT_TO_FLOAT(pot);
	float delay_min_s, delay_max_s;
	int range_val = pots[2];
	if (range_val == 0) { // Long
		delay_min_s = 0.200f;
		delay_max_s = 0.800f;
	} else if (range_val == 2) { // Short
		delay_min_s = 0.050f;
		delay_max_s = 0.400f;
	} else { // Medium
		delay_min_s = 0.100f;
		delay_max_s = 0.600f;
	}
	float ratio = delay_max_s / delay_min_s;
	return delay_min_s * pow2(time_val * log2f(ratio)) * 1000.0f;
}

static float echo_tone_display(signed char pot)
{
	const signed char *pots = echo_effect.pot_values[echo_effect.seq & 1];
	float tone_val = linear_pot(pot, 0, 1);
	float tone_mult = pow2((tone_val - 0.5f) * LOG2_10);
	int model_val = pots[7];
	if (model_val < 0) model_val = 0;
	if (model_val > 2) model_val = 2;
	float playback_fc = echo_models[model_val].playback_fc;
	float eff_playback_fc = playback_fc * tone_mult;
	if (eff_playback_fc < 400.0f) eff_playback_fc = 400.0f;
	if (eff_playback_fc > 18000.0f) eff_playback_fc = 18000.0f;
	return eff_playback_fc;
}

// These return literal pot value for enumerations
static float echo_model_pot(signed char pot) { return pot; }
static float echo_range_pot(signed char pot) { return pot; }
static float echo_mode_pot(signed char pot) { return pot; }

static inline void echo_init(signed char pot[10])
{
	echo.target_blend = echo_blend(pot[0]);
	echo.target_sustain = echo_sustain(pot[1]);

	// Range mapping: 0=Long, 1=Medium, 2=Short
	int range_val = pot[2];
	if (range_val == 0) { // Long
		echo.delay_min_s = 0.200f;
		echo.delay_max_s = 0.800f;
	} else if (range_val == 2) { // Short
		echo.delay_min_s = 0.050f;
		echo.delay_max_s = 0.400f;
	} else { // Medium
		echo.delay_min_s = 0.100f;
		echo.delay_max_s = 0.600f;
	}

	float time_val = echo_time_pot(pot[3]);
	// Set target delay from time_val mapping logarithmically
	float ratio = echo.delay_max_s / echo.delay_min_s;
	echo.target_delay_s = echo.delay_min_s * pow2(time_val * log2f(ratio));

	echo.target_record_level = echo_record(pot[4]);
	echo.target_tone = echo_tone_pot(pot[5]);
	echo.target_wow = echo_wow(pot[6]);

	// Model mapping: 0=EP-1, 1=EP-2, 2=EP-3
	int model_val = pot[7];
	echo.model_idx = model_val;
	echo.model = &echo_models[echo.model_idx];

	// Mode mapping: 0=Preamp, 1=Normal, 2=SOS
	int mode_val = pot[8];
	echo.sos_mode = 0;
	echo.preamp_only = 0;
	if (mode_val == 0) {
		echo.preamp_only = 1; // Preamp
	} else if (mode_val == 2) {
		if (echo.model->sos_capable) {
			echo.sos_mode = 1; // SOS
		}
	}

	// Initialize LPFs
	biquad_lpf(&echo.record_lpf, echo.model->record_fc, 0.707f);
	biquad_lpf(&echo.playback_lpf, echo.model->playback_fc, 0.707f);
	biquad_lpf(&echo.feedback_lpf, echo.model->feedback_fc, 0.707f);
	biquad_lpf(&echo.noise_lpf, 4000.0f, 0.707f);
}

// Simple one-pole smoother
static inline float echo_smooth(float current, float target, float coeff)
{
	return current + coeff * (target - current);
}

static inline float echo_step(float in)
{
	// Smooth parameters
	float coeff = 0.0008f;
	echo.blend = echo_smooth(echo.blend, echo.target_blend, coeff);
	echo.sustain = echo_smooth(echo.sustain, echo.target_sustain, coeff);
	echo.record_level = echo_smooth(echo.record_level, echo.target_record_level, coeff);
	echo.tone = echo_smooth(echo.tone, echo.target_tone, coeff);
	echo.wow = echo_smooth(echo.wow, echo.target_wow, coeff);

	// Use model specific delay smoothing
	// Delay smoothing speed is part of the model character. EP-1's heavy tape
	// head moves slowly (long, wandering glide); EP-3's precision motor responds
	// more crisply. This coeff is what makes the pitch glide feel different
	// per model, not just the delay time range.
	echo.delay_s = echo_smooth(echo.delay_s, echo.target_delay_s, echo.model->delay_smooth);

	// 1. Preamp waveshaper
	// The Echoplex signal always passes through the preamp, so the coloration
	// is present even with BLEND fully dry. This is historically accurate.
	// "Preamp Only" mode exploits exactly this: dry signal through the preamp, no echo added.
	float preamp_out;
	if (echo.model->preamp_asymmetry > 0.05f) {
		// Tube: asymmetric waveshaper generates even-order harmonics (biased tanhf).
		float arg = in * echo.model->preamp_drive + echo.model->preamp_asymmetry;
		preamp_out = limit_value(arg) - limit_value(echo.model->preamp_asymmetry);
	} else {
		// Solid state: symmetric odd-harmonic clipping (FET/transistor character).
		preamp_out = limit_value(in * echo.model->preamp_drive);
	}

	// 2. Wow and Flutter
	// Wow is the slow pitch wobble from motor speed variation. A secondary
	// ~0.15 Hz oscillator drifts the wow rate slightly over time, creating
	// "wandering" motor irregularity rather than a steady periodic cycle.
	// Wow and flutter depths are proportional to the current delay time: the
	// longer the loop, the more absolute pitch variation you get.
	float wow_mod_freq = 0.15f;
	echo.wow_mod_phase += fraction_to_u32(wow_mod_freq / SAMPLES_PER_SEC);
	float wow_mod = fastsincos(u32_to_fraction(echo.wow_mod_phase)).sin;

	float wow_rate_hz = echo.model->wow_rate * (1.0f + 0.25f * wow_mod);
	echo.wow_phase += fraction_to_u32(wow_rate_hz / SAMPLES_PER_SEC);

	float wow_depth_s = echo.delay_s * 0.005f * echo.model->wow_scale * echo.wow;
	float wow_offset_s = fastsincos(u32_to_fraction(echo.wow_phase)).sin * wow_depth_s;

	echo.flutter_phase += fraction_to_u32(echo.model->flutter_rate / SAMPLES_PER_SEC);
	float flutter_depth_s = echo.delay_s * 0.0005f * echo.model->flutter_scale * echo.wow;
	float flutter_offset_s = fastsincos(u32_to_fraction(echo.flutter_phase)).sin * flutter_depth_s;

	float read_pos_s = echo.delay_s + wow_offset_s + flutter_offset_s;

	// Convert to samples
	float read_pos = read_pos_s * SAMPLES_PER_SEC;

	// clamp read_pos
	if (read_pos < 1.0f) read_pos = 1.0f;
	if (read_pos > 65534.0f) read_pos = 65534.0f;

	// 3. Read
	// Read BEFORE write so we get the old loop content. If we wrote first,
	// we would read the newly-written sample -- zero-latency feedback, which
	// is wrong and also sounds terrible.
	float tape_out = sample_array_read(read_pos, &echo.idx, echo.array);

	// 4. Playback LPF (dynamic based on tone pot)
	float tone_mult = pow2((echo.tone - 0.5f) * LOG2_10);
	float eff_playback_fc = echo.model->playback_fc * tone_mult;
	if (eff_playback_fc < 400.0f) eff_playback_fc = 400.0f;
	if (eff_playback_fc > 18000.0f) eff_playback_fc = 18000.0f;

	biquad_lpf(&echo.playback_lpf, eff_playback_fc, 0.707f);
	float wet = biquad_step(&echo.playback_lpf, tape_out);

	// 5. Feedback path
	// Playback signal routes back to the record input. The dedicated feedback
	// LPF cuts a bit more high end on every pass around the loop -- that's
	// the whole reason Echoplex repeats start relatively bright and gradually
	// turn dark and murky. This one filter IS the sound.
	float fb = biquad_step(&echo.feedback_lpf, wet);

	// 6. Write
	// New signal (record path) mixed with feedback, then soft-clipped.
	// In SOS mode, feedback is pegged at 1.0 (erase head bypassed: the loop
	// accumulates indefinitely). The SUSTAIN knob is effectively bypassed in
	// SOS; RECORD LEVEL controls how aggressively new signal layers over old.
	// limit_value() keeps the write value bounded even when feedback_amt >= 1.0.
	float record_signal = biquad_step(&echo.record_lpf, preamp_out * echo.record_level);
	float feedback_amt = echo.sos_mode ? 1.0f : echo.sustain * echo.model->max_feedback;

	sample_array_write(limit_value(record_signal + fb * feedback_amt), &echo.idx, echo.array);

	// 7. Tape hiss
	// Low-level filtered white noise. Scaling by noise_floor
	// keeps it inaudible at typical listening levels while still adding air.
	float hiss = biquad_step(&echo.noise_lpf, echo_get_white_noise()) * echo.model->noise_floor;
	wet += hiss;

	// 8. Equal-power blend
	// In Preamp-Only mode, set wet to zero and blend to 0, so the output is
	// pure preamp_out -- the coloration without any echo.
	// Equal-power crossfade ensures total signal power stays constant across the blend sweep.
	float blend = echo.preamp_only ? 0.0f : echo.blend;
	if (echo.preamp_only) wet = 0.0f;

	struct sincos gains = fastsincos(blend * 0.25f);
	float dry_gain = gains.cos;
	float wet_gain = gains.sin;

	float out = preamp_out * dry_gain + wet * wet_gain;

	// Status indication using intense
	echo_effect.intense = (echo.sos_mode || echo.preamp_only) ? 1 : 0;

	return out;
}

static const char *const echo_model_names[] = { "EP-1", "EP-2", "EP-3", NULL };
static const char *const echo_range_names[] = { "Long", "Medium", "Short", NULL };
static const char *const echo_mode_names[] = { "Preamp", "Normal", "SOS", NULL };

static struct effect echo_effect = {
	.name = "EchoKing",
	.short_name = "ECHO",
	.init = echo_init,
	.step = echo_step,
	.pots = {
		{ "Blend", desc_none, echo_blend },
		{ "Sustain", desc_none, echo_sustain },
		{ "Range", desc_none, echo_range_pot, 1, echo_range_names },
		{ "Time", desc_ms, echo_time_display },
		{ "Record", desc_none, echo_record },
		{ "Tone", desc_Hz, echo_tone_display },
		{ "WowFlut", desc_none, echo_wow },
		{ "Model", desc_none, echo_model_pot, 1, echo_model_names },
		{ "Mode", desc_none, echo_mode_pot, 1, echo_mode_names },
	}
};
