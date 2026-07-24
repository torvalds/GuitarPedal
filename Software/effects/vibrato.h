// NAME: Vibrato [VIB]
// PRIORITY: 70
// POT: "Rate" FREQUENCY(0.1 8.0) = 2.0 Hz
// POT: "Depth" LINEAR(0.0 5.0) = 0.875 ms
// Vibrato: LFO-modulated delay line for a classic Doppler shift.
// Blending dry signal with the wet signal produces a rich chorus-like effect.
// At 100% wet only the pitch-shifted signal is audible; fun for rotary speaker emulation.

#define VIBRATO_CENTER_SAMPLES (6.0f * SAMPLES_PER_MSEC)

static struct {
	struct lfo_state lfo;
	float depth;		// delay amplitude in samples
	unsigned int idx;
	float samples[1024];	// ~21ms at 48kHz; max read = center(6ms) + depth(5ms) = 528 samples
} vibrato;

static void vibrato_init(unsigned char pot[10])
{
	set_lfo_freq(&vibrato.lfo, vibrato_pot0(pot[0]));
	vibrato.depth = vibrato_pot1(pot[1]) * SAMPLES_PER_MSEC;

}

static float vibrato_step(float in)
{
	float d = VIBRATO_CENTER_SAMPLES + vibrato.depth * lfo_step(&vibrato.lfo, lfo_sinewave);
	sample_array_write(in, &vibrato.idx, vibrato.samples);
	float wet = sample_array_read(d, &vibrato.idx, vibrato.samples);
	return wet;
}
