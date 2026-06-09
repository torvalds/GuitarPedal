// NAME: Graphic EQ [EQ]
// GRAPH: eq_graph
// PRIORITY: 120
// POT: "  31 Hz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "  62 Hz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: " 125 Hz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: " 250 Hz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: " 500 Hz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "1.0 kHz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "2.0 kHz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "4.0 kHz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "8.0 kHz" LINEAR(-20.0 20.0) = 0.0 dB
// POT: " 16 kHz" LINEAR(-20.0 20.0) = 0.0 dB
#define EFF_ENABLE_STEPS ((int)SAMPLES_PER_SEC/10)

struct {
	struct biquad_coeff coeff[10];
	struct biquad_state state[10];
} eq;

// Linear gain depending on pot value (now in dB, so we must calculate pow2)
static float eq_pot_A(float db)
{
	return pow2(db * 0.332192809f);
}

// Q depending on pot value: more extreme gain
// uses higher Q. Random choices here...
static inline float eq_pot_Q(int idx, unsigned char pot[10])
{
	unsigned char prev, curr, next;
	prev = idx ? pot[idx-1] : pot[0];
	next = (idx < 9) ? pot[idx+1] : pot[9];
	curr = pot[idx];
	int diff = abs(prev-curr) + abs(next-curr);
	return 0.707 + (diff / 300.0);
}

#include "eq-w0.h"

#define calc_eq_coeff(type, x, pot, coeff) \
	_biquad_##type(coeff, EQ_W0[x], eq_pot_Q(x,pot), eq_pot_A(eq_pot##x(pot[x])))

static void eq_init(unsigned char pot[10])
{
	struct biquad_coeff *c = eq.coeff;

	calc_eq_coeff(loshelf, 0, pot, c+0);
	calc_eq_coeff(peaking, 1, pot, c+1);
	calc_eq_coeff(peaking, 2, pot, c+2);
	calc_eq_coeff(peaking, 3, pot, c+3);
	calc_eq_coeff(peaking, 4, pot, c+4);
	calc_eq_coeff(peaking, 5, pot, c+5);
	calc_eq_coeff(peaking, 6, pot, c+6);
	calc_eq_coeff(peaking, 7, pot, c+7);
	calc_eq_coeff(peaking, 8, pot, c+8);
	calc_eq_coeff(hishelf, 9, pot, c+9);
}

static float eq_step(float in)
{
	const struct biquad_coeff *c = eq.coeff;
	struct biquad_state *s = eq.state;

	float val = _biquad_step(c+0, s+0, in);
	val = _biquad_peaking_step(c+1, s+1, val);
	val = _biquad_peaking_step(c+2, s+2, val);
	val = _biquad_peaking_step(c+3, s+3, val);
	val = _biquad_peaking_step(c+4, s+4, val);
	val = _biquad_peaking_step(c+5, s+5, val);
	val = _biquad_peaking_step(c+6, s+6, val);
	val = _biquad_peaking_step(c+7, s+7, val);
	val = _biquad_peaking_step(c+8, s+8, val);
	return _biquad_step(c+9, s+9, val);
}

static float biquad_mag_sq(const struct biquad_coeff *c, const struct sincos w0, const struct sincos w2)
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

static int eq_magnitude(int x, void *arg)
{
	// Map x (0..127) to frequency 20Hz .. 17.5kHz
	float freq = 20 * pow2(x / 13.0);

	struct sincos w0 = fastsincos(freq / SAMPLES_PER_SEC);
	struct sincos w2 = fastsincos((2.0f * freq) / SAMPLES_PER_SEC);

	float mag_sq = 1.0f;
	struct biquad_coeff c;
	unsigned char *pot = arg;

	calc_eq_coeff(loshelf,0,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,1,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,2,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,3,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,4,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,5,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,6,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,7,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(peaking,8,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);
	calc_eq_coeff(hishelf,9,pot,&c); mag_sq *= biquad_mag_sq(&c,w0,w2);

	float mag = sqrtf(mag_sq);
	if (mag < 0.0001f) mag = 0.0001f;

	// Convert linear gain to dB
	float db = 20.0f * log10f(mag);

	// Map dB to Y pixels [0..64].
	// y=25 is 1x gain (0 dB).
	// Let's map +20dB to y=0 and -20dB to y=50.
	int y = 32 - lrintf(db * 1.6f); // 32 / 20 = 1.6

	return y;
}

//
// Graph the frequency response and put a mark at
// the current active frequency
//
static void eq_graph(struct effect *effect, int active, unsigned char pots[10])
{
	sh1106_rectangle(0,0,128,64,rect_clear);
	sh1106_graph(0, 128, 0, 63, eq_magnitude, pots);

	// 0..9 onto the right frequencies
	int x = 9 + active * 13;
	int y = eq_magnitude(x,  pots);
	if (y > 61)
		y = 61;
	sh1106_rectangle(x-2,y-2,5,5,rect_clear);
}
