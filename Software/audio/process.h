//
// Do the 'sample to float' and 'float to sample' processing
// together with basic noise gating
//

#define FLOAT_TO_SAMPLE_MULTIPLIER (0x80000000 / 1.0)

static int clipping;

// Random buffer size. Note that we only expose
// half the data in the buffer so that we don't
// need to worry about new input overwriting
// the part of the buffer we're looking at.
//
// 256 samples is about 5ms worth of data at 48kHz
#define OUTPUT_SIZE 512
#define OUTPUT_MASK (OUTPUT_SIZE-1)

static struct {
	unsigned phase;
	volatile unsigned head, tail;
	volatile s32 buf[OUTPUT_SIZE * 2];
} output;

//
// The audio board with the TAC5112 seems to return -1.0..1.0
// for a -1.75V .. +1.75V signal swing (3.5V peak-to-peak)
//
// But then the *output* for a -1.0..1.0 signal is the
// expected 1Vrms: -1.41 .. +1.41V (2.828V peak-to-peak)
//
// I may be doing something wrong on the analog board, or I'm
// possibly missing some TAC5112 setup detail.
//
// In the meantime, this strange SAMPLE_TO_FLOAT_MULTIPLIER
// exists to correct for whatever I'm doing wrong.
//
// The intent here is that all our internal audio processing
// is based on a 1Vrms voltage scale.

#define SAMPLE_TO_FLOAT_MULTIPLIER (3.45 / 2.82843 / 0x80000000)

static inline float process_input(s32 sample)
{
	float val = sample * SAMPLE_TO_FLOAT_MULTIPLIER;
	if (tuner_mode) {
		analyze_process_sample(val);
		val = 0.0;
	}
	return val;
}

// Be careful about FP overflows around +1.0
static inline s32 convert_output(float out)
{
	s32 res = (s32)rintf(out * FLOAT_TO_SAMPLE_MULTIPLIER);
	if (out > 0.99) {
		clipping = 1;

		// Be careful about FP overflows close to +1.0,
		// because even just under 1.0 can round to the
		// max negative integer result
		if (res < 0 || out >= 1.0) {
			clipping = 1;
			return 0x7fffffff;
		}
	} else if (out < -1.0) {
		clipping = 1;
		return 0x80000000;
	}
	return res;
}

static inline s32 process_output(float out, s32 dry)
{
	s32 wet = convert_output(out);
	s32 left, right;

	switch (settings.usb_output) {
	case LR_None: return wet;
	case LR_Wet: left = right = wet; break;
	case LR_Dry: left = right = dry; break;
	default: left = wet; right = dry; break;
	}
	unsigned idx = (output.head & OUTPUT_MASK) * 2;
	output.buf[idx] = left;
	output.buf[idx + 1] = right;
	output.head++;
	return wet;
}

static inline unsigned output_buffer_size(void)
{
	unsigned nr = output.head - output.tail;
	if (nr > OUTPUT_SIZE/2)
		nr = OUTPUT_SIZE/2;
	return nr;
}

static inline unsigned get_output_samples(s32 *buffer, unsigned nr)
{
	unsigned head = output.head;
	unsigned tail = output.tail;

	// If more than 75% of the buffer is filled, we
	// have lost sync, and we will just restart at
	// the half buffer mark.
	unsigned max = head - tail;
	if (max > 3 * OUTPUT_SIZE / 4) {
		max = OUTPUT_SIZE / 2;
		tail = head - max;
	}

	// This is the max we'll copy
	//
	// Note that we keep 'output.tail' as
	// the full 32-bit value so that we can
	// tell if the head has gone way past.
	if (nr > max)
		nr = max;
	output.tail = tail + nr;

	tail &= OUTPUT_MASK;
	unsigned batch = nr;
	if (tail + batch > OUTPUT_SIZE) {
		batch = OUTPUT_SIZE - tail;
		memcpy(buffer, (void *)(output.buf + tail * 2), batch * 2 * sizeof(s32));
		buffer += batch * 2;
		batch = nr - batch;
		tail = 0;
	}
	memcpy(buffer, (void *)(output.buf + tail * 2), batch * 2 * sizeof(s32));
	return nr;
}
