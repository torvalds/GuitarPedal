//
// Freeverb: Schroeder-Moorer reverberator converted here for mono 48 kHz.
// Algorithm by Jezar at Dreampoint, released as public domain.
//
// Topology: 12 parallel feedback comb filters (FBCF) fed directly by the input,
// summed and passed through 6 series Schroeder allpass filters.  Each FBCF has
// a one-pole LP in its feedback path ("damp").  Each comb's read pointer is also
// modulated by a low-frequency oscillator to break up the fixed resonant peaks
// that cause metallic ringing in long tails (see LFO section below).
//
// Delay lengths are the canonical Freeverb 44100 Hz values scaled to 48000 Hz,
// extended with 4 additional comb delays and 2 additional allpass delays.
//
// ROOMSIZE sets all 12 comb feedback coefficients to the same g in [0.70, 0.98]:
//   g = roomsize * 0.28 + 0.70   (Freeverb's roomscale and roomoffset constants)
//
// DAMP sets the one-pole LP pole in [0.1, 0.5].  Higher DAMP = heavier HF rolloff
// in the feedback path.  Range starts at 0.1 rather than 0 (fully undamped tails
// are rarely useful; this keeps the low end of the pot from being dead space).
//
// MIX controls a linear dry/wet blend.  fixedgain (0.010) scales the comb input;
// scalewet (1.5) and scaledry (1.2) give unity dry gain at 0% wet.
//

#define REVERB_COMB_SIZE  2048
#define REVERB_COMB_MASK  ((unsigned)(REVERB_COMB_SIZE - 1))
#define REVERB_AP_SIZE    1024
#define REVERB_AP_MASK    ((unsigned)(REVERB_AP_SIZE - 1))
#define REVERB_FIXEDGAIN  0.010f  // scaled down from 0.015 * 8/12 for 12 combs
#define REVERB_SCALEWET   1.5f
#define REVERB_SCALEDRY   1.2f
#define REVERB_MOD_DEPTH  6.0f   // +-6 samples (~0.125 ms) max comb modulation depth

// Canonical Freeverb 44100 Hz comb delays scaled to 48000 Hz (multiply by 48/44.1),
// plus 4 additional delays continuing the spacing pattern, all < 2048.
static const unsigned reverb_comb_L[12] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760,
                                             1831, 1907, 1979, 2045 };
// Canonical Freeverb 44100 Hz allpass delays scaled to 48000 Hz,
// plus 2 additional delays continuing the descending sequence.
static const unsigned reverb_ap_L[6]   = {  605,  480,  371,  245,  153,   87 };

// Quadrature phasor LFO: each sample, (s,c) is rotated by (ds,dc) -- a 2x2
// rotation matrix multiply, no sincos call at runtime.  4 LFOs round-robin to
// 12 combs; rates geometrically spaced (~3:2) to prevent beating; init phases
// staggered a quarter-cycle apart so they are uncorrelated at startup (and on
// every pot change, since reverb_init resets them).
struct reverb_lfo { float s, c, ds, dc; };

static const float reverb_lfo_rates[4]  = { 0.21f, 0.31f, 0.46f, 0.67f };
static const float reverb_lfo_phases[4] = { 0.0f,  0.25f, 0.5f,  0.75f };

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
	struct reverb_comb    combs[12];
	struct reverb_allpass allpasses[6];
	struct reverb_lfo     lfo[4];
	float wet_level;
	float damp;               // LP pole in [0, 0.4]
	float g;                  // feedback gain shared by all combs
} reverb_state;
// Note: the pico-sdk .data copy-from-flash silently fails for very large objects,
// seemingly zeroing them. So, all fields (including .delay) are set in reverb_init.

// The Cortex-M33 FPU does not enable flush-to-zero by default. Without this,
// subnormal floats in the comb feedback path cause a ~100x slowdown on ARM
// once the reverb tail decays into the sub-1e-38 range during silence.
static inline float reverb_flush(float x)
{
	union { float f; unsigned int i; } u = { x };
	return (u.i & 0x7f800000u) ? x : 0.0f;
}

static float reverb_mix(signed char pot)      { return POT_TO_FLOAT(pot); }
static float reverb_roomsize(signed char pot) { return linear_pot(pot, 0.70f, 0.98f); }
static float reverb_damp(signed char pot)     { return linear_pot(pot, 0.1f, 0.5f); }

static void reverb_init(signed char pot[10])
{
	reverb_state.g         = reverb_roomsize(pot[1]);
	reverb_state.damp      = reverb_damp(pot[2]);
	reverb_state.wet_level = reverb_mix(pot[0]);

	for (int i = 0; i < 12; i++)
		reverb_state.combs[i].delay = reverb_comb_L[i];
	for (int i = 0; i < 6; i++)
		reverb_state.allpasses[i].delay = reverb_ap_L[i];
	for (int i = 0; i < 4; i++) {
		struct sincos ph  = fastsincos(reverb_lfo_phases[i]);
		struct sincos rot = fastsincos(reverb_lfo_rates[i] / SAMPLES_PER_SEC);
		reverb_state.lfo[i] = (struct reverb_lfo){ ph.sin, ph.cos, rot.sin, rot.cos };
	}
}

static float reverb_step(float in)
{
	float input = in * REVERB_FIXEDGAIN;
	float damp  = reverb_state.damp;
	float g     = reverb_state.g;
	float wet   = 0.0f;

	// Advance each LFO one step: complex multiply by the unit rotation (ds,dc).
	for (int i = 0; i < 4; i++) {
		struct reverb_lfo *lfo = &reverb_state.lfo[i];
		float new_c = lfo->c * lfo->dc - lfo->s * lfo->ds;
		lfo->s = lfo->s * lfo->dc + lfo->c * lfo->ds;
		lfo->c = new_c;
	}

	for (int i = 0; i < 12; i++) {
		struct reverb_comb *c = &reverb_state.combs[i];
		float mod    = reverb_state.lfo[i % 4].s;
		float depth  = (float)(REVERB_COMB_MASK - c->delay);  // headroom before buffer wrap
		if (depth > REVERB_MOD_DEPTH) depth = REVERB_MOD_DEPTH;
		float fdelay = (float)c->delay + mod * depth;
		unsigned id  = (unsigned)fdelay;
		float frac   = fdelay - (float)id;
		unsigned ri  = c->idx;
		float out    = c->buf[(ri - id)     & REVERB_COMB_MASK] * (1.0f - frac)
		             + c->buf[(ri - id - 1) & REVERB_COMB_MASK] * frac;
		c->filterstore = reverb_flush(out + damp * (c->filterstore - out)); // one-pole LP
		c->buf[c->idx++ & REVERB_COMB_MASK] = input + g * c->filterstore;
		wet += out;
	}

	// Schroeder allpass: output = buf - input; feedback = input + 0.5*buf.
	for (int i = 0; i < 6; i++) {
		struct reverb_allpass *a = &reverb_state.allpasses[i];
		float buf = reverb_flush(a->buf[(a->idx - a->delay) & REVERB_AP_MASK]);
		a->buf[a->idx++ & REVERB_AP_MASK] = wet + 0.5f * buf;
		wet = buf - wet;
	}

	float w = reverb_state.wet_level;
	return w * REVERB_SCALEWET * wet + (1.0f - w) * REVERB_SCALEDRY * in;
}

static struct effect reverb_effect = {
	.name       = "Reverb",
	.short_name = "VERB",
	.init       = reverb_init,
	.step       = reverb_step,
	.pots = {
		{ "Mix",      desc_none, reverb_mix,      -65, NULL },  // w=0.175
		{ "Room",     desc_none, reverb_roomsize,   0, NULL },  // g=0.84 (medium room)
		{ "Damp",     desc_none, reverb_damp,      15, NULL },  // pole=0.33
	}
};
