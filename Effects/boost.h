// NAME: Boost [BOOST]
// PRIORITY: 40
// ABOUT: Just louder, with the low and high ends trimmed so it
// ABOUT: stays tight. In front of an amp that is already working
// ABOUT: it is what pushes it over.
// POT: "Boost" LINEAR(0.0 40.0) = 0.0 dB
// INFO: How much gain goes in, in dB. This is the whole effect;
// INFO: everything else here shapes what it does.
// POT: "Level" LINEAR(-40.0 0.0) = 0.0 dB
// INFO: Output level, in dB, after the boost. Turn it down when the
// INFO: boost is loud enough to be a problem rather than a sound.
// POT: "Basscut" FREQUENCY(10.0 200.0) = 30 Hz
// INFO: Rolls off everything below this. Keeping the low end out is
// INFO: what stops a boost turning into mud. Leave it near the bottom
// INFO: unless it does; the top of the range is a thin sound.
// POT: "Highcut" FREQUENCY(1.0 20.0) = 3.4 kHz
// INFO: Rolls off above this. Leave it near the top unless the boost
// INFO: has gone brittle, and bring it down until it has not.
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

static float boost_step(float in)
{
	float out = in * boost.mult;

	out = biquad_step(&boost.basscut, out);
	out = biquad_step(&boost.highcut, out);

	float level = boost.level;
	for (;;) {
		float val = fabsf(out);
		if (val <= level)
			break;

		boost_effect.intense = 1;
		val = (3*level - val) / 2;
		out = signbit(out) ? -val : val;
	}
	return out;
}
