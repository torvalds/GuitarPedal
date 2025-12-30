enum lfo_type {
	lfo_triangle,
	lfo_sawtooth,
	lfo_sinewave
};
void set_lfo_type(enum lfo_type);
void set_lfo_freq(float freq);
void set_lfo_ms(float ms);
float lfo_step(void);
