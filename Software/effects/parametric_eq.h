// NAME: Parametric EQ [PEQ]
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
