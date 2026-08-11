// NAME: Compressor [COMPRESSOR]
// PRIORITY: 30
// POT: "Level" LINEAR(-60.0 -10.0) = -35.0 dB
// POT: "Attack" LINEAR(2.0 100.0) = 15.0 ms
// POT: "Release" LINEAR(50.0 500.0) = 150.0 ms
// POT: "Ratio" EXPONENTIAL(1.0 20.0) = 4.8 x
// POT: "Boost" LINEAR(0.0 24.0) = 6.0 dB
//
// Compressor Effect
//

static struct {
	// Pot values (pre-computed for step calculation convenience)
	float level, ratio, boost;

	// Envelope follower
	struct envelope envelope;

	// Compressor state
	float compression;
} compressor = {
	.level = 1.0,
	.compression = 1.0
};

static inline void compressor_init(unsigned char pot[10])
{
	float level_db = compressor_level_pot(pot);
	compressor.level = db_to_level(level_db);

	float attack_ms = compressor_attack_pot(pot);
	float release_ms = compressor_release_pot(pot);
	envelope_init(&compressor.envelope, attack_ms, release_ms);

	float ratio = compressor_ratio_pot(pot);
	compressor.ratio = 1.0f - (1.0f / ratio);

	float boost_db = compressor_boost_pot(pot);
	compressor.boost = db_to_level(boost_db);
}

// This may be overkill, but we're doing this by the book
static inline float mypow(float a, float b)
{
	return pow2(log2f(a) * b);
}

static inline float compressor_step(float in)
{
	// Envelope follower
	float env = envelope_step(&compressor.envelope, in);

	// Compression calculation.
	//
	// When we go over the compression level, we calculate a
	// target compression multiplier:
	//
	//	target = (level / env) ^ (1 - 1/ratio)
	//
	float target = 1.0f;
	if (env > compressor.level) {
		target = mypow(compressor.level / env, compressor.ratio);
		compressor_effect.intense = 1;
	}

	// ... and then we smooth the compression factor to prevent clicks
	compressor.compression = linear(0.01f, compressor.compression, target);

	return in * compressor.compression * compressor.boost;
}
