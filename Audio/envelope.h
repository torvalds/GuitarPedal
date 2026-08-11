// Simple envelope follower helper
struct envelope {
	float value, attack, release;
};

static inline void envelope_init(struct envelope *env, float attack_ms, float release_ms)
{
	env->attack = time_constant(attack_ms);
	env->release = time_constant(release_ms);
}

static inline float envelope_step(struct envelope *env, float in)
{
	float curr = fabsf(in), prev = env->value;
	float coef = (curr > prev) ? env->attack : env->release;
	float value = linear(coef, curr, prev);
	env->value = value;
	return value;
}
