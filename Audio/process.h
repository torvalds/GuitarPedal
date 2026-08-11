//
// Do the 'sample to float' and 'float to sample' processing
// together with basic noise gating
//

#define FLOAT_TO_SAMPLE_MULTIPLIER (0x80000000 / 1.0)

// Random buffer size. Note that we only expose
// half the data in the buffer so that we don't
// need to worry about new input overwriting
// the part of the buffer we're looking at.
//
// 256 samples is about 5ms worth of data at 48kHz
#define USB_OUTPUT_SIZE 512
#define USB_OUTPUT_MASK (USB_OUTPUT_SIZE-1)

static struct {
	unsigned phase;
	unsigned head, tail;
	raw_sample_t buf[USB_OUTPUT_SIZE];
} usb_output;

//
// The internal scale: 1.0 is one volt RMS of sine.
//
// The codec's full scale is what sets this, and it is the same at
// both ends.  tac5112.h puts VREF at 2.75V (register 0x4d) and
// runs everything single-ended, and at that VREF the TAC5242 is
// 2Vrms differential, so 1Vrms single-ended - in and out alike.
//
// So a full-scale sample is a 1Vrms sine, which peaks at 1.4142V,
// and 'raw / 2^31' is already the internal float we want: its peak
// equals the RMS volts of a sine.  A 1Vrms sine peaks at 1.0
// internally, which is what "0dBFS = 1Vrms" means and what
// level_to_dbfs() reports against.  No correction factor.
//
// It used to have one:
//
//	#define SAMPLE_TO_FLOAT_MULTIPLIER (3.45 / 2.82843 / 0x80000000)
//
// on the grounds that the input reached full scale at 3.45Vpp while
// the output produced 2.828Vpp, and the comment here said outright
// that it existed "to correct for whatever I'm doing wrong".  The
// two ends really are symmetric, so there was nothing to correct
// and the correction was itself the error: it put 1.725dB of gain
// between the input and the output, which is a pedal that is not
// unity gain even bypassed.
//
// Measured, output patched back to input on one board: the round
// trip read +1.499dB before this and -0.226dB after, and the
// remainder is real analog loss rather than arithmetic.  See
// Validation/test-analog.py.
//
// The observation that constant was justified with is still true
// and is worth keeping: a 90mVpp 110Hz sine is 31.8mVrms and reads
// -30dBFS, on an early TAC5112 board and a current TAC5242 one
// alike.  It was the *inference* that was wrong.  That reading says
// the internal peak equals the input's RMS volts, which is one
// equation in two unknowns - it is satisfied by 3.45Vpp full scale
// with a 1.2198 multiplier and by 2.828Vpp with no multiplier at
// all, since 2.828 x 1.2198 is 3.45.  The datasheet picks between
// them, and picks the second.
//
// Measured directly once the multiplier was gone, which is when the
// question stops being degenerate: with this at 1.0 the captured
// value *is* Vpeak/Vfs_peak, so a known input reads the full scale
// off.  A 500mVpp 440Hz sine from a signal generator:
//
//	reads			-15.22 dBFS
//	implied full scale	2.8855Vpp = 1.0202Vrms
//	datasheet		1.0000Vrms	(+0.17dB)
//
// and the old 3.45Vpp figure would have put that same input at
// -16.78 dBFS, which is 1.55dB from where it actually landed.  The
// remaining 2% is inside a bench generator's own accuracy and is
// not worth chasing further.
//
// Swept with the same generator, 500mVpp, and the whole input is one
// high-pass and nothing else:
//
//	  20 Hz	 -2.73 dB	 4000 Hz  -0.04 dB
//	  40 Hz	 -0.93 dB	10000 Hz  -0.09 dB
//	 160 Hz	 -0.08 dB	16000 Hz  -0.05 dB
//	 440 Hz	  0.00 dB
//	1000 Hz	 +0.02 dB
//
// A single pole at 18.6Hz fits all eight points with an rms error of
// 0.036dB, and a two-pole fit is twice as bad and biased - so whichever
// of the board's coupling cap and the codec's own AC coupling is
// higher, the other is far enough down to be invisible.  From 160Hz to
// 16kHz the spread is 0.109dB peak to peak, which is the repeatability
// of the measurement rather than anything the pedal is doing: there is
// no high-frequency rolloff below Nyquist at all.
//
// What that says about the low end, which was the open question: -0.22dB
// at a guitar's low E, -0.81dB at a bass low E, -1.35dB at a five-string
// low B.  The corner is below the bottom of hearing.
//
// The scale holds at a second amplitude: 2Vpp at 440Hz implies 1.0218
// Vrms against 500mVpp's 1.0202, the same to 0.014dB across a 4x change.
// That rules out a generator offset and any nonlinearity, and does not
// rule out a proportional generator error - a bench source reading 2%
// low everywhere looks exactly like this, and separating the two wants a
// meter on the generator rather than more captures.
//
// It also accounts for the round trip.  The DAC and the ADC are
// specified alike but measure 2% apart, so sending a signal out and
// reading it back loses that 2% - which is the -0.226dB
// test-analog.py reports, leaving the cable itself essentially
// lossless rather than the -0.23dB being a cable at all.
//
#define SAMPLE_TO_FLOAT_MULTIPLIER (1.0 / 0x80000000)

static inline sample_t process_input(raw_sample_t sample)
{
	sample_t val = {
		.left = sample.left * SAMPLE_TO_FLOAT_MULTIPLIER,
		.right = sample.right * SAMPLE_TO_FLOAT_MULTIPLIER
	};

	if (tuner_mode) {
		analyze_process_sample(val);
		val.left = val.right = 0.0;
	}
	return val;
}

//
// Convert a nominal -1.0..1.0 signal to a full-scale s32 sample.
//
// Anything at or past full scale gets pinned to the end of the range.
// The test has to be on the *input*: the FP->int conversion is only
// defined for values that already fit, and testing the result can't
// work anyway, since it comes back around into range at +-2.0, +-4.0...
//
// Below full scale there's nothing to worry about: the largest float
// under 1.0 is (1 - 2^-24), so the scaled value tops out at 2^31 - 128
// and the conversion cannot overflow.
//
static inline s32 convert_output(float out)
{
	if (fabsf(out) >= 1.0f) {
		output_clipped = 1;
		return out > 0.0f ? INT32_MAX : INT32_MIN;
	}
	return lrintf(out * FLOAT_TO_SAMPLE_MULTIPLIER);
}

static inline raw_sample_t process_output(sample_t out, raw_sample_t dry)
{
	raw_sample_t wet = {
		.left = convert_output(out.left),
		.right = convert_output(out.right)
	};
	raw_sample_t usb;

	switch (settings.usb_output) {
	case LR_None: return wet;
	case LR_Wet: usb = wet; break;
	case LR_Dry: usb = dry; break;
	default: usb.left = wet.left; usb.right = dry.left; break;
	}
	unsigned head = usb_output.head;

	// Store the sample, *then* publish it. The other way round - which
	// is what this used to do - lets cpu0 see the new head and read the
	// slot before cpu1 has written it.
	usb_output.buf[head & USB_OUTPUT_MASK] = usb;
	smp_store_release(&usb_output.head, head + 1);
	return wet;
}

static inline unsigned output_buffer_size(void)
{
	unsigned nr = usb_output.head - usb_output.tail;
	if (nr > USB_OUTPUT_SIZE/2)
		nr = USB_OUTPUT_SIZE/2;
	return nr;
}

static inline unsigned get_output_samples(s32 *buffer, unsigned nr)
{
	unsigned head = smp_load_acquire(&usb_output.head);
	unsigned tail = usb_output.tail;

	// If more than 75% of the buffer is filled, we
	// have lost sync, and we will just restart at
	// the half buffer mark.
	unsigned max = head - tail;
	if (max > 3 * USB_OUTPUT_SIZE / 4) {
		max = USB_OUTPUT_SIZE / 2;
		tail = head - max;
	}

	// This is the max we'll copy
	//
	// Note that we keep 'output.tail' as
	// the full 32-bit value so that we can
	// tell if the head has gone way past.
	if (nr > max)
		nr = max;
	usb_output.tail = tail + nr;

	tail &= USB_OUTPUT_MASK;
	unsigned batch = nr;
	if (tail + batch > USB_OUTPUT_SIZE) {
		batch = USB_OUTPUT_SIZE - tail;
		memcpy(buffer, usb_output.buf + tail, batch * sizeof(raw_sample_t));
		buffer += batch * 2;
		batch = nr - batch;
		tail = 0;
	}
	memcpy(buffer, usb_output.buf + tail, batch * sizeof(raw_sample_t));
	return nr;
}
