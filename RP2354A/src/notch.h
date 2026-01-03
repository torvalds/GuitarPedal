struct biquad notch_bq;

void notch_init(float pot1, float pot2, float pot3, float pot4)
{
	biquad_notch_filter(&notch_bq.coeff, 330, 4);
}

float notch_step(float in)
{
	return biquad_step(&notch_bq, in);
}
