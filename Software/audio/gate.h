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

static float gate_pot0_level(signed char pot)
{
	// -100dB .. -40dB is 0.01mV..10mV peak signal level
	// If your noise floor is anywhere near -100dB, you don't need the
	// gate in the first place, but maybe it's a good way to actually
	// _see_ what your noise floor is...
	return linear_pot(pot, -100.0f, -40.0f);
}

static float gate_pot1_attack(signed char pot)
{
	return linear_pot(pot, 0.0f, 10.0f);	// 0..10ms
}

static float gate_pot2_release(signed char pot)
{
	return linear_pot(pot, 50.0f, 500.0f); // 50ms to 500ms
}

static inline void gate_init(signed char pot[10])
{
	float level_db = gate_pot0_level(pot[0]);
	gate.level = db_to_level(level_db);

	float attack_ms = gate_pot1_attack(pot[1]);
	gate.attack = time_constant(attack_ms);

	float release_ms = gate_pot2_release(pot[2]);
	gate.release = time_constant(release_ms);
}

static struct effect gate_effect;

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

static struct effect gate_effect = {
	.name = "Noise Gate",
	.short_name = "GATE",
	.init = gate_init,
	.step = gate_step,
	.pots = {
		{ "Level", desc_dB, gate_pot0_level, 0 },	// -70dB
		{ "Attack", desc_ms, gate_pot1_attack, -42 },	// ~1.5ms
		{ "Release", desc_ms, gate_pot2_release, -30 },	// ~150ms
	}
};
