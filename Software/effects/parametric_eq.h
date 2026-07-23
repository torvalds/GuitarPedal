// NAME: Parametric EQ [PEQ]
// GRAPH: peq_graph
// PRIORITY: 120
// POT: "LS Freq" FREQUENCY(20.0 400.0) = 100.0 Hz
// POT: "LS Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P1 Freq" FREQUENCY(50.0 1000.0) = 250.0 Hz
// POT: "P1 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P2 Freq" FREQUENCY(100.0 4000.0) = 1000.0 Hz
// POT: "P2 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P3 Freq" FREQUENCY(400.0 10000.0) = 4000.0 Hz
// POT: "P3 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "HS Freq" FREQUENCY(1000.0 20000.0) = 8000.0 Hz
// POT: "HS Gain" LINEAR(-20.0 20.0) = 0.0 dB

#define EFF_ENABLE_STEPS ((int)SAMPLES_PER_SEC/10)

struct {
	struct biquad_coeff coeff[5];
	struct biquad_state state[5];
} peq;

static float peq_pot_A(float db)
{
	// A = 10^(db/40), which is 2^(db * LOG2_10 / 40)
	return pow2(db * (LOG2_10 / 40.0f));
}

// Fixed Q for the 5-band EQ
static const float PEQ_Q = 1.0f;

static void parametric_eq_init(unsigned char pot[10])
{
	struct biquad_coeff *c = peq.coeff;

	_biquad_loshelf(c+0, fastsincos(parametric_eq_pot0(pot[0]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot1(pot[1])));
	_biquad_peaking(c+1, fastsincos(parametric_eq_pot2(pot[2]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot3(pot[3])));
	_biquad_peaking(c+2, fastsincos(parametric_eq_pot4(pot[4]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot5(pot[5])));
	_biquad_peaking(c+3, fastsincos(parametric_eq_pot6(pot[6]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot7(pot[7])));
	_biquad_hishelf(c+4, fastsincos(parametric_eq_pot8(pot[8]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot9(pot[9])));
}

static float parametric_eq_step(float in)
{
	const struct biquad_coeff *c = peq.coeff;
	struct biquad_state *s = peq.state;

	float val = _biquad_step(c+0, s+0, in);
	val = _biquad_peaking_step(c+1, s+1, val);
	val = _biquad_peaking_step(c+2, s+2, val);
	val = _biquad_peaking_step(c+3, s+3, val);
	return _biquad_step(c+4, s+4, val);
}

static float peq_biquad_mag_sq(const struct biquad_coeff *c, const struct sincos w0, const struct sincos w2)
{
	float re_num = c->b0 + c->b1 * w0.cos + c->b2 * w2.cos;
	float im_num = c->b1 * w0.sin + c->b2 * w2.sin;
	float num = re_num * re_num + im_num * im_num;

	float re_den = 1.0f + c->a1 * w0.cos + c->a2 * w2.cos;
	float im_den = c->a1 * w0.sin + c->a2 * w2.sin;
	float den = re_den * re_den + im_den * im_den;

	if (den < 1e-12f) return num * 1e12f;
	return num / den;
}

static int peq_magnitude(int x, void *arg)
{
	// Map x (0..127) to frequency 20Hz .. 17.5kHz
	float freq = 20 * pow2(x / 13.0);

	struct sincos w0 = fastsincos(freq / SAMPLES_PER_SEC);
	struct sincos w2 = fastsincos((2.0f * freq) / SAMPLES_PER_SEC);

	float mag_sq = 1.0f;
	struct biquad_coeff c;
	unsigned char *pot = arg;

	_biquad_loshelf(&c, fastsincos(parametric_eq_pot0(pot[0]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot1(pot[1]))); mag_sq *= peq_biquad_mag_sq(&c,w0,w2);
	_biquad_peaking(&c, fastsincos(parametric_eq_pot2(pot[2]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot3(pot[3]))); mag_sq *= peq_biquad_mag_sq(&c,w0,w2);
	_biquad_peaking(&c, fastsincos(parametric_eq_pot4(pot[4]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot5(pot[5]))); mag_sq *= peq_biquad_mag_sq(&c,w0,w2);
	_biquad_peaking(&c, fastsincos(parametric_eq_pot6(pot[6]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot7(pot[7]))); mag_sq *= peq_biquad_mag_sq(&c,w0,w2);
	_biquad_hishelf(&c, fastsincos(parametric_eq_pot8(pot[8]) / SAMPLES_PER_SEC), PEQ_Q, peq_pot_A(parametric_eq_pot9(pot[9]))); mag_sq *= peq_biquad_mag_sq(&c,w0,w2);

	float mag = sqrtf(mag_sq);
	if (mag < 0.0001f) mag = 0.0001f;

	// Convert linear gain to dB
	float db = 20.0f * log10f(mag);

	// Map dB to Y pixels [0..64].
	// y=32 is 1x gain (0 dB).
	// +20dB -> y=0, -20dB -> y=64
	int y = 32 - lrintf(db * 1.6f);

	return y;
}

static void peq_graph(struct effect *effect, int active, unsigned char pots[10])
{
	sh1106_rectangle(0,0,128,64,rect_clear);
	sh1106_graph(0, 128, 0, 63, peq_magnitude, pots);

	// Mark active band
	if (active < 10) {
		int band = active / 2; // 0..4
		float freq = 0;
		switch(band) {
			case 0: freq = parametric_eq_pot0(pots[0]); break;
			case 1: freq = parametric_eq_pot2(pots[2]); break;
			case 2: freq = parametric_eq_pot4(pots[4]); break;
			case 3: freq = parametric_eq_pot6(pots[6]); break;
			case 4: freq = parametric_eq_pot8(pots[8]); break;
		}
		// x = 13 * log2(freq/20)
		int x = lrintf(13.0f * log2f(freq / 20.0f));
		int y = peq_magnitude(x, pots);
		if (y > 61) y = 61;
		if (y < 2) y = 2;
		if (x > 125) x = 125;
		if (x < 2) x = 2;
		sh1106_rectangle(x-2,y-2,5,5,rect_clear);
	}
}
