// NAME: Noise Gate [GATE]
// PRIORITY: 10
// POT: "Level" LINEAR(-100.0 -40.0) = -70.0 dB
// POT: "Attack" LINEAR(0.0 10.0) = 1.5 ms
// POT: "Release" LINEAR(50.0 500.0) = 150.0 ms
//
// Noise Gate
//
// Same envelope calculations as the compressor. I wonder if
// I should just have a combined noise gate / compressor thing?
//
// But the attack/release values are probably different.
//

static struct {
	// Pot values (pre-computed for step calculation convenience)
	float level, attack, release;

	// Signal envelope
	float env, mult;
} gate;

static inline void gate_init(unsigned char pot[10])
{
	float level_db = gate_pot0(pot[0]);
	gate.level = db_to_level(level_db);

	float attack_ms = gate_pot1(pot[1]);
	gate.attack = time_constant(attack_ms);

	float release_ms = gate_pot2(pot[2]);
	gate.release = time_constant(release_ms);
}

static inline float gate_step(float in)
{
	// Envelope follower
	float abs_val = fabsf(in);
	float prev = gate.env;
	float coef = (abs_val > prev) ? gate.attack : gate.release;
	float env = linear(coef, abs_val, prev);
	float mult = gate.mult;

	// Ramp up fairly quickly, ramp down slowly
	if (env >= gate.level) {
		mult = linear(0.01f, mult, 1.0f);
		if (mult > 0.99f)
			mult = 1.0f;
	} else {
		mult = linear(0.001f, mult, 0.0f);
		if (mult < 0.01f)
			mult = 0.0f;
		gate_effect.intense = 1;
	}
	gate.env = env;
	gate.mult = mult;
	return in * mult;
}
