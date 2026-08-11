// NAME: Reverb [REVERB]
// PRIORITY: 90
// MIX: POWER		// same as the echo, only more so
// DEFAULT_MIX: 0.18
// POT: "Room" LINEAR(0.70 0.98) = 0.88
// POT: "Damp" LINEAR(0.1 0.5) = 0.25
//
// Freeverb: Schroeder-Moorer reverberator for mono 48 kHz.
// Algorithm by Jezar at Dreampoint, released as public domain.
//
// 8 parallel FBCFs fed by the input, summed through 4 series Schroeder allpass
// filters.  Each FBCF has a one-pole LP ("damp") in its feedback path.  Each
// comb's read pointer is LFO-modulated to break up fixed resonant peaks in long
// tails (Lexicon trick).
//
// DAMP starts at 0.1 rather than 0: a fully undamped tail is rarely useful and
// makes the low end of the pot dead.
//
// This returns the wet signal and nothing else, which is what 'MIX: POWER' up
// there is for.  Stock Freeverb carries its own wet/dry and a scaledry to go
// with it, and that is what was converted from - but a wet level of its own is
// a second mix control in front of the pedal's, and the one it had was never
// connected to anything.  It read a 'wet_level' that nothing ever wrote, so the
// blend came out 0% wet: eight combs and four allpasses ran every sample and
// the answer was multiplied away.  The whole effect was a wire, from the day it
// was converted until it was measured.
//
// scalewet (1.5) is what is left of that, and stays: it is the gain that makes
// a fully wet tail sit at a sensible level against the dry it replaces.
//

#define REVERB_COMB_SIZE  2048   // must be > max comb delay (1760) + mod depth (6)
#define REVERB_COMB_MASK  ((unsigned)(REVERB_COMB_SIZE - 1))
#define REVERB_AP_SIZE    1024   // must be > max allpass delay (605); 512 is too small
#define REVERB_AP_MASK    ((unsigned)(REVERB_AP_SIZE - 1))
#define REVERB_FIXEDGAIN  0.015f  // stock Freeverb value for 8 combs
#define REVERB_SCALEWET   1.5f
#define REVERB_MOD_DEPTH  6.0f   // +-6 samples (~0.125 ms) comb read modulation

// Canonical Freeverb 44100 Hz comb delays scaled to 48000 Hz.
static const unsigned reverb_comb_L[8] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760 };
// Canonical Freeverb 44100 Hz allpass delays scaled to 48000 Hz.
static const unsigned reverb_ap_L[4]   = {  605,  480,  371,  245 };

// 4 LFOs round-robin across 8 combs; rates ~3:2 spaced to avoid beating;
// phases staggered 90 degrees to decorrelate at startup.
//
// Phase accumulators through lfo_step(), like every other modulated
// effect here.  They used to be quadrature phasors - an (s,c) pair
// rotated by a fixed (ds,dc) every sample, which is cheaper and has no
// table in it - but nothing renormalised the pair, so its magnitude
// went wherever float32 took it, and which way depended on the rate.
// At 0.21, 0.31 and 0.46 Hz the rotation's cosine rounds to exactly
// 1.0f, so the magnitude is 1 + ds*ds and the pair spirals outwards; at
// 0.67 Hz the cosine landed one ulp below one and the pair decayed to a
// tenth of its amplitude in ten minutes.
//
// The outward direction is the worse of the two.  'mod' scales
// REVERB_MOD_DEPTH, so a growing phasor walks the comb read pointer out
// of its 2048-sample buffer after about thirteen hours and past zero
// after seventeen, where the cast to unsigned is undefined behaviour.
// An accumulator cannot drift at all - it wraps, which is what a phase
// is for.
//
// Through lfo_step_X() rather than lfo_step(), because a fifth of a
// hertz does not need describing forty-eight thousand times a second.
// The real lookup happens once every 32 frames and a straight line
// joins them, which at these rates is 120dB below the modulation's own
// amplitude.  See lfo.h; it is most of what the per-sample version of
// this cost.

// In RAM rather than flash: reverb_init() runs on the audio core, which
// keeps playing while core 0 has XIP switched off to write flash.
static const float __not_in_flash("audio") reverb_lfo_rates[4]  = { 0.21f, 0.31f, 0.46f, 0.67f };
static const float __not_in_flash("audio") reverb_lfo_phases[4] = { 0.0f,  0.25f, 0.5f,  0.75f };

struct reverb_comb {
	float    buf[REVERB_COMB_SIZE];
	float    filterstore;     // one-pole LP state (the "damp" filter)
	unsigned idx;             // write head; read is (idx - delay) & mask
	unsigned delay;
};

struct reverb_allpass {
	float    buf[REVERB_AP_SIZE];
	unsigned idx;
	unsigned delay;
};

static struct {
	struct reverb_comb    combs[8];
	struct reverb_allpass allpasses[4];
	struct lfo_slow       lfo[4];
	float damp;               // LP pole in [0.1, 0.5]
	float g;                  // feedback gain shared by all combs
} reverb_state;
// All fields including .delay are set in reverb_init: pico-sdk's .data
// copy-from-flash silently zeros large objects, so don't rely on static init.

static void reverb_init(unsigned char pot[10])
{
	reverb_state.g         = reverb_room_pot(pot);
	reverb_state.damp      = reverb_damp_pot(pot);

	for (int i = 0; i < 8; i++)
		reverb_state.combs[i].delay = reverb_comb_L[i];
	for (int i = 0; i < 4; i++)
		reverb_state.allpasses[i].delay = reverb_ap_L[i];
	for (int i = 0; i < 4; i++) {
		set_lfo_freq_X(&reverb_state.lfo[i], reverb_lfo_rates[i]);
		reverb_state.lfo[i].lfo.idx =
			fraction_to_u32(reverb_lfo_phases[i]);
	}
}

static float reverb_step(float in)
{
	float input = in * REVERB_FIXEDGAIN;
	float damp  = reverb_state.damp;
	float g     = reverb_state.g;
	float wet   = 0.0f;

	// All four advance every sample, whichever combs read them.
	float lfo[4];
	for (int i = 0; i < 4; i++)
		lfo[i] = lfo_step_X(&reverb_state.lfo[i], lfo_sinewave);

	for (int i = 0; i < 8; i++) {
		struct reverb_comb *c = &reverb_state.combs[i];

		//
		// Interpolated, like every other modulated delay here.
		//
		// Truncating instead makes the read pointer jump a whole
		// sample as the LFO sweeps, and a jump is a step
		// discontinuity in the tail - the worst kind, broadband,
		// with harmonics falling off as 1/n against 1/n^2 for a
		// corner.  Eight combs at four rates spray it continuously.
		//
		// Measured by band-limiting the input to 1kHz and looking
		// above 4kHz, where a reverb that is LTI apart from a
		// sub-hertz modulation cannot legitimately put anything:
		// truncating manufactured 34.5dB of content that was not
		// in the input, interpolating manufactures none.
		//
		float d     = (float)c->delay + lfo[i % 4] * REVERB_MOD_DEPTH;
		unsigned id = (unsigned)d;
		float lo    = c->buf[(c->idx - id) & REVERB_COMB_MASK];
		float hi    = c->buf[(c->idx - id - 1) & REVERB_COMB_MASK];
		float out   = linear(d - (float)id, lo, hi);
		c->filterstore = out + damp * (c->filterstore - out);
		c->buf[c->idx++ & REVERB_COMB_MASK] = input + g * c->filterstore;
		wet += out;
	}

	// Schroeder allpass: output = buf - input; feedback = input + 0.5*buf.
	for (int i = 0; i < 4; i++) {
		struct reverb_allpass *a = &reverb_state.allpasses[i];
		float buf = a->buf[(a->idx - a->delay) & REVERB_AP_MASK];
		a->buf[a->idx++ & REVERB_AP_MASK] = wet + 0.5f * buf;
		wet = buf - wet;
	}

	return wet * REVERB_SCALEWET;
}
