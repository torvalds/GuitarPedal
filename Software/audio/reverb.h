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
// scalewet (1.5) and scaledry (1.0) give unity dry gain at 0% wet.
//

#define REVERB_COMB_SIZE  2048   // must be > max comb delay (1760) + mod depth (6)
#define REVERB_COMB_MASK  ((unsigned)(REVERB_COMB_SIZE - 1))
#define REVERB_AP_SIZE    1024   // must be > max allpass delay (605); 512 is too small
#define REVERB_AP_MASK    ((unsigned)(REVERB_AP_SIZE - 1))
#define REVERB_FIXEDGAIN  0.015f  // stock Freeverb value for 8 combs
#define REVERB_SCALEWET   1.5f
#define REVERB_SCALEDRY   1.0f
#define REVERB_MOD_DEPTH  6.0f   // +-6 samples (~0.125 ms) comb read modulation

// Canonical Freeverb 44100 Hz comb delays scaled to 48000 Hz.
static const unsigned reverb_comb_L[8] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760 };
// Canonical Freeverb 44100 Hz allpass delays scaled to 48000 Hz.
static const unsigned reverb_ap_L[4]   = {  605,  480,  371,  245 };

// Quadrature phasor: (s,c) rotated each sample by (ds,dc) -- no sincos at
// runtime.  4 LFOs round-robin across 8 combs; rates ~3:2 spaced to avoid
// beating; phases staggered 90 degrees to decorrelate at startup.
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
	struct reverb_comb    combs[8];
	struct reverb_allpass allpasses[4];
	struct reverb_lfo     lfo[4];
	float wet_level;
	float damp;               // LP pole in [0.1, 0.5]
	float g;                  // feedback gain shared by all combs
} reverb_state;
// All fields including .delay are set in reverb_init: pico-sdk's .data
// copy-from-flash silently zeros large objects, so don't rely on static init.

// Without flush-to-zero, subnormal floats in the comb feedback path cause a
// ~100x ARM slowdown once the tail decays into the sub-1e-38 range.
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

	for (int i = 0; i < 8; i++)
		reverb_state.combs[i].delay = reverb_comb_L[i];
	for (int i = 0; i < 4; i++)
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

	// Complex multiply rotates each LFO (s,c) by one step.
	for (int i = 0; i < 4; i++) {
		struct reverb_lfo *lfo = &reverb_state.lfo[i];
		float new_c = lfo->c * lfo->dc - lfo->s * lfo->ds;
		lfo->s = lfo->s * lfo->dc + lfo->c * lfo->ds;
		lfo->c = new_c;
	}

	for (int i = 0; i < 8; i++) {
		struct reverb_comb *c = &reverb_state.combs[i];
		float mod   = reverb_state.lfo[i % 4].s;
		unsigned id = (unsigned)((float)c->delay + mod * REVERB_MOD_DEPTH);
		float out   = c->buf[(c->idx - id) & REVERB_COMB_MASK];
		c->filterstore = reverb_flush(out + damp * (c->filterstore - out));
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

	float w = reverb_state.wet_level;
	return w * REVERB_SCALEWET * wet + (1.0f - w) * REVERB_SCALEDRY * in;
}

static struct effect reverb_effect = {
	.name       = "Reverb",
	.short_name = "VERB",
	.init       = reverb_init,
	.step       = reverb_step,
	.pots = {
		{ "Mix",      desc_none, reverb_mix,      -64, NULL },  // w=0.18
		{ "Room",     desc_none, reverb_roomsize,  29, NULL },  // g=0.88
		{ "Damp",     desc_none, reverb_damp,     -25, NULL },  // pole=0.25
	}
};
