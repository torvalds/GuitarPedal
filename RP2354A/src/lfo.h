#define F_STEP (TWO_POW_32/SAMPLES_PER_SEC)

enum lfo_type {
	lfo_sinewave,
	lfo_triangle,
	lfo_sawtooth,
};

struct lfo_state {
	uint idx, step;
	enum lfo_type type;
};

void set_lfo_freq(struct lfo_state *, float freq);
void set_lfo_ms(struct lfo_state *, float ms);
float lfo_step(struct lfo_state *);

// Use this for LFO initializers.
#define LFO_FREQ(x) .step = (x)*F_STEP
