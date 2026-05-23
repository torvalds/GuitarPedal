//
// "EchoKing" for Cleveland Music Co. Hothouse DIY DSP Platform
// Simplified and converted to C for the RP2350 direct pedal format
//
// A digital model of the Maestro Echoplex tape delay, using a mashup of the 
// EP-1, EP-2, and EP-3 variants to create a single "Echoplex-inspired" model with
// a modern overall sound. The EP-1 and EP-2 are the warmest and grittiest, and the 
// EP-3 is a much more aggressive, brighter sound with less wow and flutter. So,
// we borrow from the EP-3's edgier character and reduced wow and flutter, but keep
// the tube preamp character of the earlier models. Roll your own below.
//
// Three things define the sound: the preamp (always in the signal path, colouring
// the dry tone), the tape medium (bandwidth limits, soft saturation, and repeats
// that darken each pass), and the transport mechanics (wow and flutter). All three
// are modelled here.
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

//
// tanhf() approximation. Saturation happens in the Echoplex in places where
// curve shape matters: the preamp waveshapers and the feedback write-back.
//
// limit_value() from util.h is too aggressive and imprecise here. Use a
// saturation-tuned Pade [5/4] approximation of tanh, clamped at the input.
// More context at the bottom of the file, but the basic gist is that
// limit_value has no linear region and adds harmonic distortion too early.
//
static inline float echo_tanhf(float x)
{
	if (x >  3.0f) x =  3.0f;
	if (x < -3.0f) x = -3.0f;
	float x2 = x * x;
	float x4 = x2 * x2;
	return x * (945.0f + 120.0f*x2 + 2.0f*x4)
	     / (945.0f + 435.0f*x2 + 21.0f*x4);
}

//
// One-pole low-pass: y[n] = (1-a)*x[n] + a*y[n-1], 6 dB/oct rolloff.
//
// The Echoplex tone is shaped by *gentle* 1st-order rolloffs at the record,
// playback, and feedback stages. Use a one-pole here to stay true to the OG.
// More context at the bottom of the file, but the short version is that a
// biquad with a low Q cuts too much above the corner, and stacked biquads
// cut too much, too fast.
//
// Pole coeff: a = exp(-2*pi*fc/fs) = pow2(-2*pi*fc/fs / ln(2)).
// The 9.06472 constant is 2*pi/ln(2) precomputed.
//
struct echo_onepole {
	float a, z;
};

static inline void echo_onepole_set_cutoff(struct echo_onepole *lpf, float fc_hz)
{
	lpf->a = pow2(-9.06472028f * fc_hz / SAMPLES_PER_SEC);
}

static inline float echo_onepole_step(struct echo_onepole *lpf, float x)
{
	// y[n] = (1-a)*x[n] + a*y[n-1], rearranged to
	// y[n] = x + a*(y[n-1] - x) to fold into a single fma.
	lpf->z = x + lpf->a * (lpf->z - x);
	return lpf->z;
}

// Delay range. Fixed (not per-model and not exposed via a toggle), so a pair
// of constants is enough; shared by echo_init and echo_time_display.
#define ECHO_DELAY_MIN_S    0.050f
#define ECHO_DELAY_MAX_S    0.800f
// log2(0.800 / 0.050) = log2(16) = 4.0 exactly, used by the log-taper mapping.
#define ECHO_DELAY_LOG2_RATIO 4.0f

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
};

//
// Echoplex model and EP-1, 2, & 3 params reference
//
// These values are the result of lots of A/B testing against original units
// and tweaking to approximate the character of each. Use them to create your
// own Echoplex sound by mixing and matching parameters.
//
// EP-1: Tube (12AX7 single-ended), early cartridge transport. Loosest
// mechanics, strongest even-harmonic colouration, most wow and flutter.
//
// EP-2: Tube (12AX7 + 12AU7), revised circuit. The canonical Echoplex sound.
// Thick, warm, the dry tone is perceptibly coloured even without any echo.
//
// EP-3: Solid state (JFET/FET). Tighter and more focused. The preamp is famous
// as a standalone boost. Better motor regulation = much less wow and flutter.
//
// struct member       EP-1        EP-2        EP-3
// ----------------------------------------------------
// preamp_drive        1.4f        1.5f        1.2f
// preamp_asymmetry    0.22f       0.18f       0.04f
// record_fc           5500.0f     6000.0f     7500.0f
// playback_fc         4500.0f     5000.0f     6000.0f
// feedback_fc         3500.0f     4000.0f     5000.0f
// wow_scale           1.40f       1.00f       0.65f
// wow_rate            1.2f        1.3f        1.5f
// flutter_scale       1.20f       1.00f       0.60f
// flutter_rate        8.0f        9.0f        10.5f
// noise_floor         0.0005f     0.0003f     0.00015f
// max_feedback        0.95f       1.00f       1.05f
// delay_smooth        0.00006f    0.00008f    0.00012f
//
static const struct echo_model model =
{
    /*preamp_drive=*/1.27f,
    /*preamp_asymmetry=*/0.15f,
    /*record_fc=*/7500.0f,
    /*playback_fc=*/6000.0f,
    /*feedback_fc=*/5000.0f,
    /*wow_scale=*/0.85f,
    /*wow_rate=*/1.5f,
    /*flutter_scale=*/0.90f,
    /*flutter_rate=*/9.0f,
    /*noise_floor=*/0.0f, // determines strength of tape noise; none, please
    /*max_feedback=*/1.05f,
    /*delay_smooth=*/0.00012f
};

struct {
	float target_blend, blend;
	float target_sustain, sustain;
	float target_delay_s, delay_s;
	float target_record_level, record_level;
	float target_tone, tone;
	float target_wow, wow;

	int sos_mode;

	u32 wow_phase;
	u32 wow_mod_phase;
	u32 flutter_phase;

	struct echo_onepole record_lpf;
	struct echo_onepole playback_lpf;
	struct echo_onepole feedback_lpf;
	struct echo_onepole noise_lpf;

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
// Display callbacks: convert a pot to the human-friendly real-world unit
// (ms / Hz) shown on the OLED. echo_time_display mirrors the log-taper in
// echo_init; echo_tone_display mirrors the per-sample tone calc in echo_step.
// Any change to the maths there must be reflected here too.
//
static float echo_time_display(signed char pot)
{
	float time_val = POT_TO_FLOAT(pot);
	return ECHO_DELAY_MIN_S * pow2(time_val * ECHO_DELAY_LOG2_RATIO) * 1000.0f;
}

static float echo_tone_display(signed char pot)
{
	float tone_val = linear_pot(pot, 0, 1);
	float tone_mult = pow2((tone_val - 0.5f) * LOG2_10);
	float eff_playback_fc = model.playback_fc * tone_mult;
	if (eff_playback_fc < 400.0f) eff_playback_fc = 400.0f;
	if (eff_playback_fc > 18000.0f) eff_playback_fc = 18000.0f;
	return eff_playback_fc;
}

// Returns the literal pot value; the UI indexes into enum_names with it.
static float echo_mode_pot(signed char pot) { return pot; }

static inline void echo_init(signed char pot[10])
{
   	echo.target_blend = echo_blend(pot[0]);
	echo.target_sustain = echo_sustain(pot[1]);

	// Log-taper map of the time pot across ECHO_DELAY_MIN_S..MAX_S.
	float time_val = echo_time_pot(pot[2]);
	echo.target_delay_s = ECHO_DELAY_MIN_S * pow2(time_val * ECHO_DELAY_LOG2_RATIO);

	echo.target_record_level = echo_record(pot[3]);
	echo.target_tone = echo_tone_pot(pot[4]);
	echo.target_wow = echo_wow(pot[5]);

	// Mode: 0=Normal, 1=SOS
	// `echo.sos_mode = pot[6]` would also work, but let's be defensive and
	// not couple to the enum_names array.
	echo.sos_mode = (pot[6] == 1);

	// Initialize LPFs (one-pole, 6 dB/oct; see comments at bottom of file).
	echo_onepole_set_cutoff(&echo.record_lpf, model.record_fc);
	echo_onepole_set_cutoff(&echo.playback_lpf, model.playback_fc);
	echo_onepole_set_cutoff(&echo.feedback_lpf, model.feedback_fc);
	echo_onepole_set_cutoff(&echo.noise_lpf, 4000.0f);
}

static inline float echo_smooth(float current, float target, float coeff)
{
	return current + coeff * (target - current);
}

static inline float echo_step(float in)
{
	float coeff = 0.0008f;
	echo.blend = echo_smooth(echo.blend, echo.target_blend, coeff);
	echo.sustain = echo_smooth(echo.sustain, echo.target_sustain, coeff);
	echo.record_level = echo_smooth(echo.record_level, echo.target_record_level, coeff);
	echo.tone = echo_smooth(echo.tone, echo.target_tone, coeff);
	echo.wow = echo_smooth(echo.wow, echo.target_wow, coeff);

	// Slow glide on delay-time changes; avoids zipper noise and gives the
	// characteristic tape-head pitch shift when the time pot moves.
	echo.delay_s = echo_smooth(echo.delay_s, echo.target_delay_s, model.delay_smooth);

	// 1. Preamp waveshaper
	// The Echoplex signal always passes through the preamp, so the colouration
	// is present even with BLEND fully dry.
	float preamp_out;
	if (model.preamp_asymmetry > 0.05f) {
		// Tube: asymmetric waveshaper generates even-order harmonics (biased tanhf).
		float arg = in * model.preamp_drive + model.preamp_asymmetry;
		preamp_out = echo_tanhf(arg) - echo_tanhf(model.preamp_asymmetry);
	} else {
		// Solid state: symmetric odd-harmonic clipping (FET/transistor character).
		preamp_out = echo_tanhf(in * model.preamp_drive);
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

	float wow_rate_hz = model.wow_rate * (1.0f + 0.25f * wow_mod);
	echo.wow_phase += fraction_to_u32(wow_rate_hz / SAMPLES_PER_SEC);

	float wow_depth_s = echo.delay_s * 0.005f * model.wow_scale * echo.wow;
	float wow_offset_s = fastsincos(u32_to_fraction(echo.wow_phase)).sin * wow_depth_s;

	echo.flutter_phase += fraction_to_u32(model.flutter_rate / SAMPLES_PER_SEC);
	float flutter_depth_s = echo.delay_s * 0.0005f * model.flutter_scale * echo.wow;
	float flutter_offset_s = fastsincos(u32_to_fraction(echo.flutter_phase)).sin * flutter_depth_s;

	float read_pos_s = echo.delay_s + wow_offset_s + flutter_offset_s;

	float read_pos = read_pos_s * SAMPLES_PER_SEC;
	if (read_pos < 1.0f) read_pos = 1.0f;
	if (read_pos > 65534.0f) read_pos = 65534.0f;

	// 3. Read
	// Read BEFORE write so we get the old loop content. If we wrote first,
	// we would read the newly-written sample.
	float tape_out = sample_array_read(read_pos, &echo.idx, echo.array);

	// 4. Playback LPF (dynamic based on tone pot)
	float tone_mult = pow2((echo.tone - 0.5f) * LOG2_10);
	float eff_playback_fc = model.playback_fc * tone_mult;
	if (eff_playback_fc < 400.0f) eff_playback_fc = 400.0f;
	if (eff_playback_fc > 18000.0f) eff_playback_fc = 18000.0f;

	echo_onepole_set_cutoff(&echo.playback_lpf, eff_playback_fc);
	float wet = echo_onepole_step(&echo.playback_lpf, tape_out);

	// 5. Feedback path
	// Playback signal routes back to the record input. The dedicated feedback
	// LPF cuts a bit more high end on every pass around the loop; that's the
	// whole reason Echoplex repeats start reasonably bright and gradually
	// turn dark and murky. This one filter IS the tape echo sound.
	float fb = echo_onepole_step(&echo.feedback_lpf, wet);

	// 6. Write
	// New signal (record path) mixed with feedback, then soft-clipped.
	float record_signal = echo_onepole_step(&echo.record_lpf, preamp_out * echo.record_level);
	// In SOS mode, feedback is pegged at 1.0 (erase head bypassed: the loop
	// accumulates indefinitely). The SUSTAIN knob is effectively bypassed in
	// SOS; RECORD LEVEL controls how aggressively new signal layers over old.
	float feedback_amt = echo.sos_mode ? 1.0f : echo.sustain * model.max_feedback;
	// echo_tanhf() (rather than limit_value) keeps the loop bounded without
	// distorting at sub-saturation levels; see comment at bottom of file.
	sample_array_write(echo_tanhf(record_signal + fb * feedback_amt), &echo.idx, echo.array);

	// 7. Tape hiss
	// Low-level filtered white noise. Scaling by noise_floor keeps it inaudible 
	// at typical listening levels while still adding "air".
	float hiss = echo_onepole_step(&echo.noise_lpf, echo_get_white_noise()) * model.noise_floor;
	wet += hiss;

	// 8. Equal-power blend; keeps total signal power constant across the sweep.
	struct sincos gains = fastsincos(echo.blend * 0.25f);
	float dry_gain = gains.cos;
	float wet_gain = gains.sin;

	float out = preamp_out * dry_gain + wet * wet_gain;

	// Brighten the status LED in SOS so the player can see they're in
	// the indefinite-loop mode.
	echo_effect.intense = echo.sos_mode;

	return out;
}

static const char *const echo_mode_names[] = { "Normal", "SOS", NULL };

static struct effect echo_effect = {
	.name = "Tape Echo",
	.short_name = "ECHO",
	.init = echo_init,
	.step = echo_step,
	.pots = {
		{ "Blend", desc_none, echo_blend, -54 },
		{ "Sustain", desc_none, echo_sustain, -42 },
		{ "Time", desc_ms, echo_time_display, 46 },
		{ "Record", desc_none, echo_record, -57 },
		{ "Tone", desc_Hz, echo_tone_display, 19 },
		{ "WowFlut", desc_none, echo_wow },
		{ "Mode", desc_none, echo_mode_pot, 0, echo_mode_names },
	}
};

//
// echo_tanhf and echo_onepole helpers vs. limit_value and biquad_lpf.
//
// Two stages of the Echoplex signal chain are sensitive to the *shape*
// of the primitive that models them, not just their endpoints. The
// stock helpers are fine general-purpose tools but don't match the
// physical circuit topology closely enough to keep the wet repeats
// in character. If the goal is to sound like an actual tape echo, we
// should use primitives that reasonably match the real-world components.
//
// (1) Waveshaper: preamp colouring, and tape saturation on every write.
//
// A 12AX7 triode at its grid (EP-1, EP-2) and a JFET preamp (EP-3) both
// saturate as soft tanh-shaped curves: a near-linear small-signal region,
// gradual onset of compression, hard ceiling at the rails. The record-head
// bias network + tape saturation on the write operation produce the same
// shape, applied each pass around the loop.
//
// limit_value(x) = x/(1+|x|) doesn't have a linear region. The |x| term
// puts a kink in the second derivative at zero, so even tiny signals get
// harmonically distorted. A clean sustained note through a real 12AX7
// doesn't sound distorted; through limit_value it does. If we hit the
// shaper once in the preamp, then again on every trip around the
// feedback loop, the fuzz compounds.
//
// echo_tanhf is a saturation-tuned Pade [5/4] approximation,
// x*(945+120x^2+2x^4)/(945+435x^2+21x^4), with the input clamped to +/-3.
// Indistinguishable from tanh below |x|=1, within 0.1% out to |x|=2, exact
// slope at zero (small signals stay clean), and reaches exactly +/-1 at
// the clamp point so the curve and clamp meet smoothly.
//
//   x      tanh(x)    echo_tanhf    limit_value    limit_value vs. tanh
//   0.05   0.0500     0.0500        0.0476            -5%
//   0.10   0.0997     0.0997        0.0909            -9%
//   0.30   0.2913     0.2913        0.2308           -21%
//   0.50   0.4621     0.4621        0.3333           -28%
//   1.00   0.7616     0.7616        0.5000           -34%
//   2.00   0.9640     0.9646        0.6667           -31%
//   3.00   0.9951     1.0000        0.7500           -25%
//
// The right column is what matters. At a 0.1 normalised input a real
// preamp passes audio through almost cleanly; limit_value has already
// trimmed it 9% and added harmonics that the circuit wouldn't create.
// At unity input the gap is 34%, all of it added harmonic content.
// This is audibly noticeable during listening tests. The Pade 
// approximation is close enough to the real thing for our purposes, 
// and keeps the repeats sounding less woolly and distorted.
//
// (2) Lowpasses: record_lpf, playback_lpf, feedback_lpf.
//
// All three corresponding stages in the hardware are RC networks --
// one resistor, one capacitor, one pole each. None are second-order
// and none resonate:
//
//   - record amp into the tape-head bias network: single pole,
//     roughly 5-7 kHz depending on model and tape condition.
//   - playback head gap loss combined with the playback preamp:
//     dominantly first-order above the gap-loss corner; we lump
//     the two into one effective one-pole.
//   - feedback amplifier: another single pole. The "repeats go
//     dark" character is just N applications of this one stage.
//
// biquad_lpf at Q=0.707 is 12 dB/oct regardless of corner. Below the
// corner it's actually flatter than a one-pole, but above the corner
// it cuts at twice the slope. That's the wrong tradeoff here -- the
// clarity of the early repeats comes from the gentle 6 dB/oct stopband
// of an RC.
//
//   freq vs. fc     biquad (Q=0.707)    one-pole
//   fc/4              -0.0 dB             -0.3 dB
//   fc/2              -0.3 dB             -1.0 dB
//   fc                -3.0 dB             -3.0 dB
//   2*fc              -12 dB              -7 dB
//   4*fc              -24 dB              -12 dB
//   8*fc              -36 dB              -18 dB
//
// At the corner they agree. An octave above, the biquad is already 5
// dB darker; three octaves above, 18 dB darker. Stacked three deep in
// series, the wet signal loses everything above 2*fc within a couple
// of repeats. Real tape machine echoes don't darken that fast.
//
// (Phase compounds it. A one-pole contributes 45 deg at its corner; a
// biquad contributes 90. Three stages in series gives 135 vs. 270
// degrees of accumulated phase shift each trip around the loop, and 
// that smears the wet transients further.)
//
// Cost is incidental but works in our favour. biquad_step is ~5 mul
// + 4 add + a state shuffle per sample; echo_onepole_step is 1 sub
// + 1 mul + 1 add (FMA-friendly). 
//
