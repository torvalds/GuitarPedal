// NAME: Cab Sim [CAB]
// PRIORITY: 129
// POT: "Resonance" LINEAR(0 1) = 0.5
// POT: "Presence" LINEAR(0 1) = 0.5
// POT: "Axis" LINEAR(0 1) = 0.5
// POT: "Breakup" LINEAR(0 1) = 0.0
// POT: "Chug" LINEAR(0 1) = 0.0
//
// ====================================================================
// WARNING: VERY APPROXIMATE SPEAKER EMULATION!
// ====================================================================
// This is NOT an Impulse Response (IR) loader or an exact physical
// model of a speaker cone. 
//
// It is a rough IIR approximation of a classic 12-inch guitar speaker
// in a closed-back cabinet (e.g. Celestion Vintage 30) using cascaded
// biquad filters. It aims to cut the nasty "fizz" of amp distortion
// so it sounds passable through full-range studio monitors or USB
// audio, by mimicking the steep mechanical high-frequency roll-off
// and the low-end impedance bump of a real cab.
// ====================================================================

struct {
	struct biquad low_cut;     // 75 Hz HPF (speaker physical limit)
	struct biquad thump_bp;    // ~110 Hz parallel bandpass for dynamic resonance
	struct biquad scoop;       // ~400 Hz peaking (mid scoop)
	struct biquad presence;    // ~2.5 kHz peaking (speaker bite)
	struct biquad hi_cut_1;    // 1st half of 24dB/oct LPF
	struct biquad hi_cut_2;    // 2nd half of 24dB/oct LPF

	struct envelope chug_env;  // Envelope follower for low-frequency energy
	struct single_pole_state env_lp;
	struct single_pole_coeff env_coeff;
	float res_gain;            // Calculated linear gain for resonance
	float breakup_amt;
	float chug_amt;
} cabsim;

static inline void cabsim_init(unsigned char pot[10])
{
	float res_pot = cabsim_pot0(pot[0]);
	float pres_pot = cabsim_pot1(pot[1]);
	float axis_pot = cabsim_pot2(pot[2]);
	cabsim.breakup_amt = cabsim_pot3(pot[3]);
	cabsim.chug_amt = cabsim_pot4(pot[4]);

	// Calculate base resonance gain. db_to_level expects voltage gain.
	// Since we are using a parallel bandpass now, we just multiply the bandpass
	// output by (A - 1) to get the equivalent peaking boost.
	// 0 to +8dB. Amplitude A = 10^(dB/20).
	float res_db = res_pot * 8.0f;
	float res_A = db_to_level(res_db);
	cabsim.res_gain = res_A - 1.0f;
	if (cabsim.res_gain < 0.0f) cabsim.res_gain = 0.0f;

	// High cut frequency sweeps from ~3500 Hz (off-axis/dark) to ~6000 Hz (on-axis/bright)
	float hicut_freq = 3500.0f + axis_pot * 2500.0f;

	// Envelope for chug compression (fast attack, medium release)
	envelope_init(&cabsim.chug_env, 2.0f, 100.0f);
	cabsim.env_coeff = single_pole_freq(150.0f); // Lowpass for envelope tracking

	// 1. Low-end roll-off (12dB/octave HPF)
	biquad_hpf(&cabsim.low_cut, 75.0f, 0.7f);

	// 2. Cabinet Resonance thump (~110Hz) - Now a parallel bandpass
	biquad_bpf(&cabsim.thump_bp, 110.0f, 1.5f);

	// 3. Natural paper cone mid scoop (~400Hz, fixed -3dB)
	biquad_peaking(&cabsim.scoop, 400.0f, 1.0f, db_to_level(-1.5f));

	// 4. Upper-mid bite (~2500Hz)
	// (0 to +6dB max) - passing half db due to biquad_peaking math
	biquad_peaking(&cabsim.presence, 2500.0f, 1.5f, db_to_level(pres_pot * 3.0f));

	// 5. Steep 24dB/octave high-end roll-off (two cascaded 12dB/oct LPFs)
	// A simple Q=0.7 on both gives a decent 4th-order slope.
	biquad_lpf(&cabsim.hi_cut_1, hicut_freq, 0.7f);
	biquad_lpf(&cabsim.hi_cut_2, hicut_freq, 0.7f);
}

static inline float cabsim_step(float in)
{
	float out = in;

	// 1. Extract low-frequency envelope for Chug compressor
	float low_energy = single_pole_lpf(out, &cabsim.env_lp, cabsim.env_coeff);
	float env = envelope_step(&cabsim.chug_env, low_energy);

	// 2. Low-cut
	out = biquad_step(&cabsim.low_cut, out);

	// 3. Cone Breakup (Saturation)
	// We do this BEFORE the massive 110Hz thump to prevent Intermodulation Distortion (IMD).
	// If you boost bass before clipping, the bass frequencies modulate the high frequencies
	// and turn chords into indistinguishable mud.
	if (cabsim.breakup_amt > 0.0f) {
		float drive_gain = 1.0f + cabsim.breakup_amt * 40.0f;
		float driven = out * drive_gain;
		float clipped = tanhf(driven + 0.2f) - 0.197375f;
		out = clipped / (drive_gain * 0.961f);
	}

	// 4. Dynamic Thump (parallel bandpass)
	float thump_sig = biquad_step(&cabsim.thump_bp, out);
	// Chug amount determines how much the envelope reduces the resonance.
	// Since guitar signals are typically ~0.1 peak, the envelope is very small.
	// We multiply the envelope by 25.0 to scale it into a useful compression range.
	float compression = 1.0f - (env * 25.0f * cabsim.chug_amt);
	if (compression < 0.0f) compression = 0.0f;
	out += thump_sig * (cabsim.res_gain * compression);

	// 5. Scoop
	out = biquad_step(&cabsim.scoop, out);

	// 6. Presence & High Cut
	out = biquad_step(&cabsim.presence, out);
	out = biquad_step(&cabsim.hi_cut_1, out);
	out = biquad_step(&cabsim.hi_cut_2, out);

	return out;
}
