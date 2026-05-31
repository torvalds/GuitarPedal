// NAME: Preamp [PRE]
// PRIORITY: 20
// POT: "Level" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "Sat" LINEAR(0.5 4.0) = 1.2 x
// POT: "Voice" ENUM(Tube JFET) = Tube
// Two-stage cascaded triode (Tube) or famous single-stage JFET preamp model.
// SATURATION drives both waveshapers; VOICE selects topology; LEVEL trims output.

// One-pole LPF: y[n] = x[n] + a*(y[n-1] - x[n]), 6 dB/oct.
// Pole: a = pow2(-2*pi*fc / (fs*ln2)); 9.06472 = 2*pi/ln2.
struct preamp_onepole {
	float a, z;
};

static inline void preamp_onepole_set(struct preamp_onepole *f, float fc)
{
	f->a = pow2(-9.06472028f * fc / SAMPLES_PER_SEC);
}

static inline float preamp_onepole_step(struct preamp_onepole *f, float x)
{
	f->z = x + f->a * (f->z - x);
	return f->z;
}

// Tube model: two-stage 12AX7 triode, class-A biased, interstage DC block.
// Fixed operating-point asymmetry for 60's vintage character (~0.15 grid bias).
#define PREAMP_TUBE_ASYMM   0.15f
#define PREAMP_TUBE_DC_R    0.995f   // ~38 Hz HPF pole at 48 kHz
// tanhf(0.15f) and tanhf(0.045f) precomputed; saves two tanhf calls per sample.
#define PREAMP_TUBE_TANHB   0.14888f
#define PREAMP_TUBE_TANHA2  0.04496f
// Small-signal gain at default drive (1.2): stage1 * stage2 ≈ 0.885; norm targets ~1.05.
#define PREAMP_TUBE_NORM    1.186f

// JFET model: from schematic of a famous common-emitter 2N5457-like stage.
// Emitter-bypass shelf, asymmetric Class-A waveshaper, Miller-cap rolloff.
#define PREAMP_JFET_BIAS    0.12f    // class-A operating offset
#define PREAMP_JFET_MAKEUP  1.6f     // ~+4 dB output boost
#define PREAMP_JFET_SHELF_D 0.50f    // shelf depth: ~6 dB LF cut below corner
#define PREAMP_JFET_SHELF   120.0f   // Hz, emitter-bypass corner
#define PREAMP_JFET_MILLER  9000.0f  // Hz, C_cb * R_c HF rolloff
#define PREAMP_JFET_DC_R    0.997f   // ~71 Hz HPF pole at 48 kHz
// tanhf(0.12f) precomputed.
#define PREAMP_JFET_TANHB   0.11943f
// Small-signal gain at default drive (1.2): waveshaper * MAKEUP ≈ 1.89; norm targets ~1.05.
#define PREAMP_JFET_NORM    0.555f

static struct {
	float level, drive;
	int voice;

	struct { float dc_x, dc_y; } tube;

	struct {
		struct preamp_onepole shelf, miller;
		float dc_x, dc_y;
	} jfet;
} preamp = {
	.level = 1.0f,
	.drive = 1.2f,
};

static inline void preamp_init(unsigned char pot[10])
{
	preamp.level = db_to_level(preamp_pot0(pot[0]));
	preamp.drive = preamp_pot1(pot[1]);
	preamp.voice = pot[2];

	// Shelf and miller corners are fixed; computed here because pow2() needs runtime tables.
	preamp_onepole_set(&preamp.jfet.shelf,  PREAMP_JFET_SHELF);
	preamp_onepole_set(&preamp.jfet.miller, PREAMP_JFET_MILLER);
}

static inline float preamp_tube_step(float x, float drive)
{
	// Stage 1: asymmetric bias pushes the operating point off-centre on
	// the tanh curve, generating even-order harmonics (2nd-harmonic mechanism).
	float s1 = tanhf(x * (drive * 0.7f) + PREAMP_TUBE_ASYMM) - PREAMP_TUBE_TANHB;

	// Interstage DC block (~38 Hz HPF) strips stage-1 DC before stage 2
	// so the bias offsets don't accumulate across the cascade.
	float dc = s1 - preamp.tube.dc_x + PREAMP_TUBE_DC_R * preamp.tube.dc_y;
	preamp.tube.dc_x = s1;
	preamp.tube.dc_y = dc;

	// Stage 2: re-saturates the cleaned signal at reduced asymmetry.
	return tanhf(dc * (drive * 0.9f) + PREAMP_TUBE_ASYMM * 0.3f) - PREAMP_TUBE_TANHA2;
}

static inline float preamp_jfet_step(float x, float drive)
{
	// Emitter-bypass low-shelf: subtracts a fraction of the LPF output to
	// attenuate below 120 Hz by ~6 dB, matching the bypass capacitor rolloff.
	float lf = preamp_onepole_step(&preamp.jfet.shelf, x);
	float in = x - PREAMP_JFET_SHELF_D * lf;

	// Asymmetric Class-A waveshaper; BIAS offsets the operating point so
	// positive and negative swings saturate at different rates.
	// Subtracting tanhf(BIAS) removes the static DC component.
	float shaped = tanhf(in * drive + PREAMP_JFET_BIAS) - PREAMP_JFET_TANHB;

	// DC block for the drive-dependent residual offset.
	float dc = shaped - preamp.jfet.dc_x + PREAMP_JFET_DC_R * preamp.jfet.dc_y;
	preamp.jfet.dc_x = shaped;
	preamp.jfet.dc_y = dc;

	// Miller-cap HF rolloff then characteristic output boost.
	return preamp_onepole_step(&preamp.jfet.miller, dc) * PREAMP_JFET_MAKEUP;
}

static inline float preamp_step(float in)
{
	float out = (preamp.voice == 0)
		? preamp_tube_step(in, preamp.drive) * PREAMP_TUBE_NORM
		: preamp_jfet_step(in, preamp.drive) * PREAMP_JFET_NORM;
	return out * preamp.level;
}
