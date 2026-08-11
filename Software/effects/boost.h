// NAME: Boost [BOOST]
// PRIORITY: 40
// POT: "Boost" LINEAR(0.0 40.0) = 0.0 dB
// POT: "Level" LINEAR(-40.0 0.0) = 0.0 dB
// POT: "Basscut" FREQUENCY(10.0 200.0) = 30 Hz
// POT: "Highcut" FREQUENCY(1.0 20.0) = 3.4 kHz
// DEFAULT_MIX: 0.5
struct {
	float mult, level;
	struct biquad basscut, highcut;
} boost;

void boost_init(unsigned char pot[10])
{
	boost.mult = db_to_level(boost_boost_pot(pot));
	boost.level = db_to_level(boost_level_pot(pot));
	biquad_hpf(&boost.basscut, boost_basscut_pot(pot), 0.707);
	biquad_lpf(&boost.highcut, boost_highcut_pot(pot)*1000, 0.707);
}


static float fold(float in, float level)
{
	float fold_scale = 0.5;
	boost_effect.intense = 1;
	// Cap iterations to prevent infinite loop on float precision stalls
	for (int i = 0; i < 20; i++) {
		float over = (in - level) * fold_scale;

		in = level - over;
		if (in >= -level)
			return in;

		over = (in + level) * fold_scale;
		in = - level - over;
		if (in <= level)
			return in;
	}
	return in;
}

static float boost_step(float in)
{
	float out = in * boost.mult;

	out = biquad_step(&boost.basscut, out);
	out = biquad_step(&boost.highcut, out);

	if (out > boost.level)
		out = fold(out, boost.level);
	else if (out < -boost.level)
		out = -fold(-out, boost.level);

	return out;
}
