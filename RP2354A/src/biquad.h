//
// Calculate biquad coefficients for TAC5112
//
// Some of this is generic biquad, but some of it
// is then TAC5112-specific
struct biquad_coeff {
	float b0, b1, b2;
	float a1, a2;
};

struct biquad {
	struct biquad_coeff coeff;
	float w1, w2;
};

static inline float biquad_step(struct biquad *bq, float x0)
{
	float w0, y0;

	w0 = x0 - bq->coeff.a1 * bq->w1 - bq->coeff.a2 * bq->w2;
	y0 = bq->coeff.b0 * w0 + bq->coeff.b1 * bq->w1 + bq->coeff.b2 * bq->w2;
	bq->w2 = bq->w1; bq->w1 = w0;
	return y0;
}

static void biquad_lpf(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;
	float b1 = (1 - w0.cos) / a0;

	res->b0 = b1 / 2;
	res->b1 = b1;
	res->b2 = b1 / 2;
	res->a1 = -2*w0.cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_hpf(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;
	float b1 = (1 + w0.cos) / a0;

	res->b0 = b1 / 2;
	res->b1 = -b1;
	res->b2 = b1 / 2;
	res->a1 = -2*w0.cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_notch_filter(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;

	res->b0 = 1 		/ a0;
	res->b1 = -2*w0.cos	/ a0;
	res->b2 = 1		/ a0;
	res->a1 = -2*w0.cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_bpf_peak(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;

	res->b0 = Q*alpha	/ a0;
	res->b1 = 0;
	res->b2 = -Q*alpha	/ a0;
	res->a1 = -2*w0.cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_bpf(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;

	res->b0 = alpha		/ a0;
	res->b1 = 0;
	res->b2 = -alpha	/ a0;
	res->a1 = -2*w0.cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_allpass_filter(struct biquad_coeff *res, float f, float Q)
{
	struct sincos w0 = fastsincos(f/SAMPLES_PER_SEC);
	float alpha = w0.sin/(2*Q);
	float a0 = 1 + alpha;

	res->b0 = (1 - alpha)	/ a0;
	res->b1 = (-2*w0.cos)	/ a0;
	res->b2 = 1;		// Same as a0
	res->a1 = res->b1;
	res->a2 = res->b0;
}

static void bq_convert(float f, char *buf)
{
	int val = (int)rintf(f * (float)0x7fffffff);

	if (f > 0 && val < 0)
		val = 0x7fffffff;

	buf[0] = val >> 24;
	buf[1] = val >> 16;
	buf[2] = val >> 8;
	buf[3] = val;
}

static void tac_write_biquad(const struct biquad_coeff *bq, int page, int reg)
{
	char buf[1+5*4];

	buf[0] = reg;
	bq_convert(bq->b0, buf+1);
	bq_convert(0.5 * bq->b1, buf+5);
	bq_convert(bq->b2, buf+9);
	bq_convert(-0.5 * bq->a1, buf+13);
	bq_convert(-bq->a2, buf+17);

	tac5112_set_page(page);
	tac5112_write(buf, sizeof(buf));
}
