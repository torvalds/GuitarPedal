// NAME: Tape Echo [ECHO]
// PRIORITY: 80
// POT: "Blend" LINEAR(0.0 1.0) = 0.2
// POT: "Sustain" LINEAR(0.0 1.1) = 0.3
// POT: "Time" EXPONENTIAL(50.0 672.0) = 375.0 ms
// POT: "Record" LINEAR(0.1 2.0) = 0.5
// POT: "Tone" EXPONENTIAL(1739.25 17392.53) = 6400.0 Hz
// POT: "WowFlut" LINEAR(0.0 1.0) = 0.2
// POT: "Mode" ENUM(Normal SOS) = Normal
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
#define ECHO_DELAY_MAX_S    0.672f
// log2(0.672 / 0.050) ~= 3.75, used by the log-taper mapping.
#define ECHO_DELAY_LOG2_RATIO 3.75f
#define ECHO_WOW_MOD_FREQ     0.15f

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
// max_feedback        0.95f       1.00f       1.05f
// delay_smooth        0.00006f    0.00008f    0.00012f
//
static const struct echo_model model =
{
    /*preamp_drive=*/1.27f,
    /*preamp_asymmetry=*/0.15f,
    /*record_fc=*/6500.0f,
    /*playback_fc=*/5500.0f,
    /*feedback_fc=*/4500.0f,
    /*wow_scale=*/0.85f,
    /*wow_rate=*/1.5f,
    /*flutter_scale=*/0.90f,
    /*flutter_rate=*/9.0f,
    /*max_feedback=*/1.05f,
    /*delay_smooth=*/0.00012f
};

struct {
	float target_blend, blend;
	float target_sustain, sustain;
	float target_delay_s, delay_s;
	float target_record_level, record_level;
	float target_tone, tone, last_tone;
	float target_wow, wow;

	int sos_mode;

	u32 wow_phase;
	u32 wow_mod_phase;
	u32 flutter_phase;

	float preamp_dc_offset;   // tanhf(preamp_asymmetry): DC bias the waveshaper introduces
	float last_blend, dry_gain, wet_gain;

	struct echo_onepole record_lpf;
	struct echo_onepole playback_lpf;
	struct echo_onepole feedback_lpf;

	unsigned idx;
	int16_t array[32768];
} echo;

static inline void echo_init(unsigned char pot[10])
{
	echo.target_blend = echo_pot0(pot[0]);
	echo.target_sustain = echo_pot1(pot[1]);

	// The EXPONENTIAL metadata sets up the curve directly, so the pot value
	// is already in milliseconds. We just convert it to seconds.
	float time_ms = echo_pot2(pot[2]);
	echo.target_delay_s = time_ms / 1000.0f;

	echo.target_record_level = echo_pot3(pot[3]);
	echo.target_tone = echo_pot4(pot[4]);
	echo.target_wow = echo_pot5(pot[5]);

	// Mode: 0=Normal, 1=SOS
	echo.sos_mode = (pot[6] == 1);

	echo.preamp_dc_offset = tanhf(model.preamp_asymmetry);
	echo.last_blend = -1.0f;

	// Initialize LPFs (one-pole, 6 dB/oct; see comments at bottom of file).
	echo_onepole_set_cutoff(&echo.record_lpf, model.record_fc);
	echo_onepole_set_cutoff(&echo.feedback_lpf, model.feedback_fc);
}

static inline float echo_step(float in)
{
	float coeff = 0.0008f;
	echo.blend        = linear(coeff, echo.blend, echo.target_blend);
	echo.sustain      = linear(coeff, echo.sustain, echo.target_sustain);
	echo.record_level = linear(coeff, echo.record_level, echo.target_record_level);
	echo.tone         = linear(coeff, echo.tone, echo.target_tone);
	echo.wow          = linear(coeff, echo.wow, echo.target_wow);

	// Slow glide on delay-time changes; avoids zipper noise and gives the
	// characteristic tape-head pitch shift when the time pot moves.
	echo.delay_s = linear(model.delay_smooth, echo.delay_s, echo.target_delay_s);

	// 1. Preamp waveshaper
	// The Echoplex signal always passes through the preamp, so the colouration
	// is present even with BLEND fully dry.
	float preamp_out;
	if (model.preamp_asymmetry > 0.05f) {
		// Tube: asymmetric waveshaper generates even-order harmonics (biased tanhf).
		float arg = in * model.preamp_drive + model.preamp_asymmetry;
		preamp_out = tanhf(arg) - echo.preamp_dc_offset;
	} else {
		// Solid state: symmetric odd-harmonic clipping (FET/transistor character).
		preamp_out = tanhf(in * model.preamp_drive);
	}

	// 2. Wow and Flutter
	// Wow is the slow pitch wobble from motor speed variation. A secondary
	// ~0.15 Hz oscillator drifts the wow rate slightly over time, creating
	// "wandering" motor irregularity rather than a steady periodic cycle.
	// Wow and flutter depths are proportional to the current delay time: the
	// longer the loop, the more absolute pitch variation you get.
	echo.wow_mod_phase += fraction_to_u32(ECHO_WOW_MOD_FREQ / SAMPLES_PER_SEC);
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
	if (read_pos > 32766.0f) read_pos = 32766.0f;

	// 3. Read
	// Read BEFORE write so we get the old loop content. If we wrote first,
	// we would read the newly-written sample.
	float tape_out = sample_array_read_s16(read_pos, &echo.idx, echo.array);

	// 4. Playback LPF (dynamic based on tone pot)
	// Tone smooths at coeff 0.0008 -- no need to recompute the pole coefficient
	// (which calls pow2) until the value has actually shifted enough to matter.
	if (fabsf(echo.tone - echo.last_tone) > 0.001f) {
		echo.last_tone = echo.tone;
		float eff_playback_fc = echo.tone;
		if (eff_playback_fc > 18000.0f) eff_playback_fc = 18000.0f;
		echo_onepole_set_cutoff(&echo.playback_lpf, eff_playback_fc);
	}
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
	// tanhf() (rather than limit_value) keeps the loop bounded without
	// distorting at sub-saturation levels; see comment at bottom of file.
	sample_array_write_s16(tanhf(record_signal + fb * feedback_amt), &echo.idx, echo.array);

	// 7. Equal-power blend; keeps total signal power constant across the sweep.
	if (fabsf(echo.blend - echo.last_blend) > 0.001f) {
		echo.last_blend = echo.blend;
		struct sincos gains = fastsincos(echo.blend * 0.25f);
		echo.dry_gain = gains.cos;
		echo.wet_gain = gains.sin;
	}

	float out = preamp_out * echo.dry_gain + wet * echo.wet_gain;

	// Brighten the status LED in SOS so the player can see they're in
	// the indefinite-loop mode.
	echo_effect.intense = echo.sos_mode;

	return out;
}

//
// tanhf and echo_onepole helpers vs. limit_value and biquad_lpf.
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
// tanhf is the Padé approximant
//
//      x^5 + 105x^3 + 945x
//     --------------------
//     15x^4 + 420x^2 + 945
//
// with the output clamped to ±1.
//
// Indistinguishable from tanh below |x|=1 (precise to 6.5 digits),
//  4.7 digits (0.002%) out to ±2
//  3.4 digits (0.040%) out to ±3
//  2.9 digits (0.136%) everywhere
// and an exact slope at zero (so small signals stay clean)
//
// In contrast, the simplistic limit_value() approximation had a 9%
// error already at a value of 0.1. At a 0.1 normalised input a real
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
