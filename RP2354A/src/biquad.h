//
// Calculate biquad coefficients for TAC5112
//
// Some of this is generic biquad, but some of it
// is then TAC5112-specific
struct biquad {
	float b0, b1, b2;
	float a1, a2;
};

static void biquad_lpf(struct biquad *res, float f, float Q)
{
	float w0 = 2*M_PI*f/SAMPLES_PER_SEC;
	float alpha = sinf(w0)/(2*Q);
	float cos = cosf(w0);
	float a0 = 1 + alpha;
	float b1 = (1 - cos) / a0;

	res->b0 = b1 / 2;
	res->b1 = b1;
	res->b2 = b1 / 2;
	res->a1 = -2*cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_hpf(struct biquad *res, float f, float Q)
{
	float w0 = 2*M_PI*f/SAMPLES_PER_SEC;
	float alpha = sinf(w0)/(2*Q);
	float cos = cosf(w0);
	float a0 = 1 + alpha;
	float b1 = (1 + cos) / a0;

	res->b0 = b1 / 2;
	res->b1 = -b1;
	res->b2 = b1 / 2;
	res->a1 = -2*cos	/ a0;
	res->a2 = (1 - alpha)	/ a0;
}

static void biquad_notch_filter(struct biquad *res, float f, float Q)
{
	float w0 = 2*M_PI*f/SAMPLES_PER_SEC;
	float alpha = sinf(w0)/(2*Q);
	float a0 = 1 + alpha;

	res->b0 = 1 		/ a0;
	res->b1 = -2*cosf(w0)	/ a0;
	res->b2 = 1		/ a0;
	res->a1 = -2*cosf(w0)	/ a0;
	res->a2 = (1 - alpha)	/ a0;
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

static void tac_write_biquad(const struct biquad *bq, int page, int reg)
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

//
// Notch filter for testing
// Set a 330Hz notch filter with Q=4.
//
// This is absolutely ridiculous, but good for seeing
// that it's enabled (set the signal generator to 330Hz
// and see it basically disappear).
static void tac_silly_notch(void)
{
	struct biquad bq;
	biquad_notch_filter(&bq, 330, 4);
	tac_write_biquad(&bq, BIQUAD_IN1);
	biquad_lpf(&bq, 1000, 1);
	tac_write_biquad(&bq, BIQUAD_IN2);
	biquad_hpf(&bq, 220, 1);
	tac_write_biquad(&bq, BIQUAD_IN3);
}
