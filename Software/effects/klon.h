// NAME: Klonlike [KLON]
// PRIORITY: 50
// POT: "Gain" LINEAR(0 1) = 0.20
// POT: "Treble" LINEAR(0 1) = 0.50
// POT: "Output" LINEAR(0 1) = 0.40

// Klon pedal originally by Bryan Leavelle <bryanleavelle@gmail.com>
//
// Circuit modeled:
//   Charge pump (18V headroom) -> input buffer -> op-amp driving germanium
//   diodes (1N34A) to ground for hard clipping -> clean/dirty blend that tracks
//   gain knob -> treble control with 1.7kHz presence peak.
//
// The Klon's signature: the clean/dirty blend is NOT a mix knob -- it's
// wired to a dual-gang gain pot. Low gain = mostly clean with a touch of edge.
// High gain = 100% clipped. The clean signal grounds out as gain increases.

/* ------------------------------------------------------------------ */
/*  DC blocker — 1-pole HP at ~20Hz                                   */
/* ------------------------------------------------------------------ */

// NOTE! Bryan's original code didn't have the 2*pi correction, and so
// the alleged 20Hz filtering was actually a high-pass filter at around
// 3.2Hz.
//
// The real Klon Centaur has a 100nF input blocking capacitor with a
// 1M resistor to ground, so the DC blocking is actually more like a
// 1.5Hz high-pass filter.
//
// The output DC blocking is a 4.7uF cap with a 100k pulldown, which
// is even lower, but at that point we're so far away from any audio
// frequencies that it doesn't matter at all and we'll just use this
// for both cases

static inline float klon_dc_step(struct single_pole_state *state, float x)
{
	return single_pole_hpf(x, state, single_pole_rc(1e6, 100e-9));
}

struct {
	float drive, treble, level;
	struct single_pole_state dc_in;			/* DC blocking at input */
	struct single_pole_state dc_out;		/* DC blocking at output */
	struct single_pole_state in_hp;			/* 30Hz coupling cap */
	struct single_pole_state pre_lp;		/* 15kHz input bandwidth */
	struct biquad tone_hs;      			/* treble control — hi shelf @ 2kHz */
	struct biquad pres_pk;      			/* presence peak @ 1.7kHz */
} klon;

void klon_init(unsigned char pot[10])
{
	klon.drive = klon_pot0(pot[0]);
	klon.treble = klon_pot1(pot[1]);		// 0 = dark, 1 = bright
	klon.level = klon_pot2(pot[2]);

	float hs_db = (klon.treble - 0.5f) * 12.0f;	// -6 to +6 dB
	float peaking_db = klon.treble * 6.0;		//  0 to +6 dB (original effectively doubled the boost)

	// Single-pole RC filters for coupling and bandwidth
	// initialized implicitly to 0 by static allocation, no init needed here

	biquad_highshelf(&klon.tone_hs, 2000.0, 0.7, db_to_level(hs_db));
	biquad_peaking(&klon.pres_pk, 1700.0, 1.5, db_to_level(peaking_db));
}

float klon_step(float in)
{
	float drive = klon.drive;
	float level = klon.level;
	float pre, boost, ge_clip, clean_amt, dirty_amt, mixed, y;

	/* Input conditioning */
	pre = klon_dc_step(&klon.dc_in, in);

	pre = single_pole_hpf(pre, &klon.in_hp, single_pole_freq(30.0));	/* coupling cap */
	pre = single_pole_lpf(pre, &klon.pre_lp, single_pole_freq(15000.0));	/* input bandwidth */

	/* Op-amp gain — 18V charge pump gives ~2x headroom vs 9V pedals */
	boost = 1.0f + drive * drive * 55.0f;
	pre = pre * boost;

	/*
	 * Germanium diode pair (1N34A) to ground — 0.3V forward voltage
	 * (silicon is 0.7V — germanium clips softer, rounder, more compressed).
	 * Hard clipping to ground gives even harmonic content from the soft knee.
	 */
	ge_clip = tanhf(pre * 0.45f) * 1.8f;

	/*
	 * THE KLON'S SIGNATURE: clean/dirty blend tracks the gain knob via a
	 * dual-gang pot.
	 * Low drive = mostly clean with a whisper of edge.
	 * High drive = 100% clipped, clean signal is fully grounded out.
	 */
	clean_amt = 1.0f - drive;
	dirty_amt = drive;
	mixed = in * clean_amt + ge_clip * dirty_amt;

	/* Post-clip tone shaping */
	y = biquad_step(&klon.tone_hs, mixed);		/* treble shelf */
	y = biquad_step(&klon.pres_pk, y);		/* 1.7kHz presence peak */
	y = klon_dc_step(&klon.dc_out, y);		/* remove clipping DC offset */

	return y * (0.3f + level * 1.5f);
}
