// NAME: Boost [BOOST]
// PRIORITY: 40
// POT: "Boost" LINEAR(0.0 40.0) = 0.0 dB
// POT: "Level" LINEAR(-40.0 0.0) = 0.0 dB
// POT: "Basscut" FREQUENCY(10.0 200.0) = 30 Hz
// POT: "Highcut" FREQUENCY(1.0 20.0) = 3.4 kHz
// POT: "Mix" LINEAR(0.0 1.0) = 0.5
struct {
	float mult, level, mix;
	struct biquad basscut, highcut;
} boost;

void boost_init(unsigned char pot[10])
{
	boost.mult = db_to_level(boost_pot0(pot[0]));
	boost.level = db_to_level(boost_pot1(pot[1]));
	biquad_hpf(&boost.basscut, boost_pot2(pot[2]), 0.707);
	biquad_lpf(&boost.highcut, boost_pot3(pot[3])*1000, 0.707);
	boost.mix = boost_pot4(pot[4]);
}


static float fold(float in, float level)
{
	float fold_scale = 0.5;
	boost_effect.intense = 1;
	for (;;) {
		float over = (in - level) * fold_scale;

		in = level - over;
		if (in >= -level)
			return in;

		over = (in + level) * fold_scale;
		in = - level - over;
		if (in <= level)
			return in;
	}
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

	return linear(boost.mix, in, out);
}
