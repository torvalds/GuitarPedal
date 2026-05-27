//
// Freeverb: Schroeder-Moorer reverberator converted here for mono 48 kHz.
// Algorithm by Jezar at Dreampoint, released as public domain.
//
// Topology: 8 parallel feedback comb filters (FBCF) fed directly by the input,
// summed and passed through 4 series Schroeder allpass filters.  Each FBCF has
// a one-pole LP in its feedback path ("damp").
//
// Delay lengths are the canonical Freeverb 44100 Hz values scaled to 48000 Hz.
//
// ROOMSIZE sets all 8 comb feedback coefficients to the same g in [0.70, 0.98]:
//   g = roomsize * 0.28 + 0.70   (Freeverb's roomscale and roomoffset constants)
//
// DAMP sets the one-pole LP pole in [0, 0.4] (Freeverb's scaledamp = 0.4).
// Higher DAMP = heavier HF rolloff in the feedback path.
//
// MIX controls a linear dry/wet blend.  fixedgain (0.015) scales the comb input;
// scalewet (3.0) and scaledry (2.0) normalise the wet and dry levels.
//

#define REVERB_COMB_SIZE  2048
#define REVERB_COMB_MASK  ((unsigned)(REVERB_COMB_SIZE - 1))
#define REVERB_AP_SIZE    1024
#define REVERB_AP_MASK    ((unsigned)(REVERB_AP_SIZE - 1))
#define REVERB_FIXEDGAIN  0.015f
#define REVERB_SCALEWET   3.0f
#define REVERB_SCALEDRY   2.0f

// Canonical Freeverb 44100 Hz comb delays scaled to 48000 Hz (multiply by 48/44.1).
static const unsigned reverb_comb_L[8] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760 };
// Canonical Freeverb 44100 Hz allpass delays scaled to 48000 Hz.
static const unsigned reverb_ap_L[4]   = {  605,  480,  371,  245 };

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
static float reverb_damp(signed char pot)     { return linear_pot(pot, 0.0f, 0.4f); }

static void reverb_init(signed char pot[10])
{
	reverb_state.g         = reverb_roomsize(pot[1]);
	reverb_state.damp      = reverb_damp(pot[2]);
	reverb_state.wet_level = reverb_mix(pot[0]);

	for (int i = 0; i < 8; i++)
		reverb_state.combs[i].delay = reverb_comb_L[i];
	for (int i = 0; i < 4; i++)
		reverb_state.allpasses[i].delay = reverb_ap_L[i];
}

static float reverb_step(float in)
{
	float input = in * REVERB_FIXEDGAIN;
	float damp  = reverb_state.damp;
	float g     = reverb_state.g;
	float wet   = 0.0f;

	for (int i = 0; i < 8; i++) {
		struct reverb_comb *c = &reverb_state.combs[i];
		float out = c->buf[(c->idx - c->delay) & REVERB_COMB_MASK];
		c->filterstore = reverb_flush(out + damp * (c->filterstore - out)); // one-pole LP
		c->buf[c->idx++ & REVERB_COMB_MASK] = input + g * c->filterstore;
		wet += out;
	}

	// Schroeder allpass: output = buf - input; feedback = input + 0.5*buf.
	for (int i = 0; i < 4; i++) {
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
		{ "Mix",      desc_none, reverb_mix,      -50, NULL },  // 25% wet
		{ "Room",     desc_none, reverb_roomsize,   0, NULL },  // g=0.84 (medium room)
		{ "Damp",     desc_none, reverb_damp,       0, NULL },  // pole=0.2
	}
};
