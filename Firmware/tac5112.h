//
// One byte, discarded.  Used twice: once to find out whether there is a
// TAC5112 on the bus at all, and once after the reset to wait until it is
// answering again.  Timed out rather than blocking, because a board with
// nothing there is the normal case and a bus held down is not worth
// hanging over.
//
static int tac5112_read(void)
{
	unsigned char rxdata;
	return i2c_read_timeout_us(TAC5112_I2C, &rxdata, 1, false, 2000);
}

static inline bool tac5112_write(const unsigned char *data, int len)
{
	return i2c_write_blocking(TAC5112_I2C, data, len, false) == len;
}

//
// Stops at the first refusal.  Carrying on would leave the codec half
// configured either way, but it would also leave every later write
// failing silently and nothing to say which one went wrong.
//
static bool __tac5112_array_write(const unsigned char arr[][2], int nr)
{
	for (int i = 0; i < nr; i++) {
		if (!tac5112_write(arr[i], 2)) {
			report_status("TAC5112 setup failed");
			return false;
		}
	}
	return true;
}
#define tac5112_array_write(arr) __tac5112_array_write(arr, ARRAY_SIZE(arr))

static void tac5112_set_page(int page)
{
	static int current = -1;
	if (page != current) {
		unsigned char bytes[2] = { 0, page };
		current = page;
		tac5112_write(bytes, 2);
	}
}

static void bq_convert(float f, unsigned char *buf)
{
	int val = lrintf(f * (float)0x7fffffff);

	if (f > 0 && val < 0)
		val = 0x7fffffff;

	buf[0] = val >> 24;
	buf[1] = val >> 16;
	buf[2] = val >> 8;
	buf[3] = val;
}

static inline void tac_write_biquad(const struct biquad_coeff *bq, int page, int reg)
{
	unsigned char buf[1+5*4];

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
// How far outside 1.31 a section's numerator reaches.
//
// A boost always reaches outside it somewhere and a cut never does, but
// by how much depends on where the band sits rather than on how much
// boost it is: the same +15dB peak reaches 5.62 at 3kHz and 1.001 at
// 24Hz, because down there the poles and zeros crowd together near z=1
// and every coefficient sits near 1 whatever the gain.  The denominator
// of a stable section always fits once it is halved.
//
// This is a bound on storing the coefficients and on nothing else.  It
// is not the section's gain - scaling to fit does not normalise the
// response - and it says nothing about what the codec does with a
// signal between its own sections.
//
// Returned as the peak rather than as a scale, because what to do about
// it is a question about the whole channel and not about one section.
//
static float tac_numerator_peak(const struct biquad_coeff *bq)
{
	float peak = fabsf(bq->b0);

	if (fabsf(0.5f * bq->b1) > peak)
		peak = fabsf(0.5f * bq->b1);
	if (fabsf(bq->b2) > peak)
		peak = fabsf(bq->b2);
	return peak;
}

//
// A channel's digital volume, in the half-decibel steps the register
// counts.
//
// Two scales, because the two paths do not share one: the ADC's zero is
// 161 and the DAC's is 201, both in half-decibel steps, and both mute at
// zero.  The DAC has an A and a B register for each channel and both are
// written, since on this board the pair is one output.
//
// Steps rather than decibels because the caller has already divided the
// numerators by exactly what this restores, and a float dB here would
// round a second time and undo that.  Clamping is still possible - three
// bands at +15 dB ask for more than the DAC's register can count - and
// then the filter is right and the channel is quiet, which is the way
// round to be wrong.
//
static void tac_set_dvol(enum hwtone_path path, int ch, int half_db)
{
	static const unsigned char adc_reg[2] = { 0x52, 0x57 };
	static const unsigned char dac_reg[2][2] = {
		{ 0x67, 0x69 }, { 0x6e, 0x70 }
	};
	bool dac = path == HWTONE_PLAYBACK;
	int v = (dac ? 201 : 161) + half_db;
	unsigned char buf[2];

	if (v < 1)
		v = 1;
	if (v > 255)
		v = 255;
	buf[1] = v;

	tac5112_set_page(0);
	if (!dac) {
		buf[0] = adc_reg[ch];
		tac5112_write(buf, 2);
		return;
	}
	for (int i = 0; i < 2; i++) {
		buf[0] = dac_reg[ch][i];
		tac5112_write(buf, 2);
	}
}

//
// How much makeup to ask for, in the register's own steps.
//
// Rounded away from unity rather than to nearest, so the step is never
// smaller than what the sections gave away: the numerators are then
// divided by at least as much as they needed, and dividing a numerator
// further always still fits.
//
static int tac_dvol_steps(float carried)
{
	float steps = -40.0f * log10f(carried);
	int whole = (int)steps;

	if (carried >= 1.0f)
		return 0;
	return steps > (float)whole ? whole + 1 : whole;
}

//
// Where a channel's three biquads live.  The allocation is round-robin
// across the device's four channels, so channel 1 gets filters 1, 5 and
// 9 - see SLAAEH6, and ADC_DSP_BQ_CFG/DAC_DSP_BQ_CFG in tac5112_init()
// for the three-per-channel setting that puts them there.
//
struct tac_biquad_slot { unsigned char page, reg; };

static const struct tac_biquad_slot tac_adc_biquad[2][3] = {
	{ { 8, 0x08 }, { 8, 0x58 }, { 9, 0x30 } },	// channel 1
	{ { 8, 0x1c }, { 8, 0x6c }, { 9, 0x44 } },	// channel 2
};
static const struct tac_biquad_slot tac_dac_biquad[2][3] = {
	{ { 15, 0x08 }, { 15, 0x58 }, { 16, 0x30 } },	// channel 1
	{ { 15, 0x1c }, { 15, 0x6c }, { 16, 0x44 } },	// channel 2
};

//
// Core 0, from the main loop.  Does nothing until something moves.
//
// Whether there is a codec to write to is the caller's to know: this is
// included before hardware.h, which is where the probe lives.
//
// No barrier anywhere: prepare() wrote want[] on this core too.
//
// Only the sections that moved are written.  With one band under a
// finger that is one section of three, which is what makes a step every
// couple of milliseconds affordable - and taking small steps is the
// whole point, because the noise scales with how far the poles jump
// rather than with how often they are written.
//
// 'running' is whether the effect should be doing anything, and it
// picks the target rather than skipping the write: a filter in the
// codec keeps filtering, so switching it off is a walk down to 0 dB and
// takes the same care as any other move.
//
static void hwtone_task(struct hwtone *ht, enum hwtone_path path,
			bool running)
{
	const struct tac_biquad_slot (*slot)[3];
	struct biquad_coeff send[3];
	bool moved[3];
	bool any = false;
	float carried = 1.0f;
	float scale[3];
	int half_db;
	int i, ch;

	if (time_us_64() < ht->next_us)
		return;
	ht->next_us = time_us_64() + HWTONE_STEP_US;

	//
	// The first set goes in whole.  There is nothing to ease away
	// from at boot, and easing away from silence would be audible in
	// its own right.
	//
	if (!ht->live_valid) {
		for (i = 0; i < 3; i++) {
			ht->live[i] = hwtone_target(ht, i, running);
			any = moved[i] = true;
		}
		ht->live_valid = true;
	} else {
		for (i = 0; i < 3; i++) {
			struct hwtone_band want = hwtone_target(ht, i, running);

			any |= moved[i] = hwtone_approach(&ht->live[i], &want);
		}
	}
	if (!any)
		return;

	//
	// What each section's numerator is divided by, worked out across
	// all three before any of them is written.
	//
	// Dividing a numerator scales that section's response, so what
	// decides the channel's level is the product over the three and
	// not what any one of them does.  Each is divided only as far as
	// it has to be, and a later section with room to spare is scaled
	// back up by as much as the earlier ones gave away - so a boost
	// and a cut that cancel need no makeup at all, where dividing
	// each section on its own would have taken the boost out and
	// asked the volume control to put it back.
	//
	// 'carried' never rises above 1, so the restoring only ever
	// happens after the attenuating and no point inside the channel
	// is hotter than the designed filter would make it.
	//
	for (i = 0; i < 3; i++) {
		float peak;

		scale[i] = 1.0f / carried;
		hwtone_design(&send[i], i, &ht->live[i]);
		peak = tac_numerator_peak(&send[i]);
		if (scale[i] * peak > 1.0f)
			scale[i] = 1.0f / peak;
		carried *= scale[i];
	}

	//
	// What the volume control will actually give back, which is a
	// whole number of half-decibel steps and not what the sections
	// asked for.  Divide by exactly that instead, so the two cancel
	// rather than nearly cancelling - otherwise the channel sits up to
	// a quarter of a decibel away from the level it was asked for, and
	// by a different amount for every setting.
	//
	// The difference is one scalar on the whole channel, so it can go
	// on any one section and the first is as good as any.  It only ever
	// makes that numerator smaller.
	//
	half_db = tac_dvol_steps(carried);
	scale[0] *= db_to_level(-0.5f * half_db) / carried;

	for (i = 0; i < 3; i++) {
		send[i].b0 *= scale[i];
		send[i].b1 *= scale[i];
		send[i].b2 *= scale[i];

		//
		// A band that did not move can still need writing, because
		// what it is divided by depends on what the sections before
		// it gave away.
		//
		if (scale[i] != ht->live_scale[i]) {
			ht->live_scale[i] = scale[i];
			moved[i] = true;
		}
	}

	slot = path == HWTONE_PLAYBACK ? tac_dac_biquad : tac_adc_biquad;
	for (ch = 0; ch < 2; ch++) {
		for (i = 0; i < 3; i++) {
			if (moved[i])
				tac_write_biquad(&send[i], slot[ch][i].page,
						 slot[ch][i].reg);
		}
	}

	//
	// And what the sections gave away, for the volume control to put
	// back.  Never an attenuation: the sections only ever divide.
	//
	for (ch = 0; ch < 2; ch++)
		tac_set_dvol(path, ch, half_db);
}

// TAC5112 Datasheet 9.2.5:
// Example Device Register Configuration Script for EVM Setup
// Stereo differential AC-coupled analog recording and line output playback
//
// Except we do I2S instead of TDM, and single-ended mono input/output
//
// The analog bypass doesn't work the way I expected: it bypasses IN1P to
// OUT1M and IN1M to OUT1P.
//
// I have no idea why the analog bypass switches the "polarity" of the
// signals, but it means it won't work on my board that doesn't connect
// IN1M to anything.
//
// There is presumably some reason why TI did this, but I find it rather
// surprising
static void tac5112_init(void)
{
	// Nothing here on a current board, which straps the codec instead
	if (tac5112_read() < 0)
		return;

	static const unsigned char tac_reset[][2] = {
		{ 0x00, 0x00 },	// Page 0
		{ 0x01, 0x01 }, // SW reset
	};
	if (!tac5112_array_write(tac_reset))
		return;

	// Wait for it to take effect
	sleep_ms(10);
	tac5112_read();

	static const unsigned char regwrite[][2] = {
		{ 0x00, 0b00000000 },	// Page 0
		{ 0x02, 0b00000001 },	// Exit Sleep Mode with DREG and VREF Enabled
		//
		// GPIO1 reaches the MCU on minimal and nothing reads it yet.
		// Out of reset it is an interrupt output driving weak-high,
		// which fights the MCU's pull-down for tens of microamps
		// forever; disabled, that pull-down holds the line low for
		// nothing.  No board here uses the pin for anything else.
		//
		// 0x32 makes it an interrupt again - and the MCU end then
		// wants a pull-up rather than its default pull-down, because
		// RP2350-E9 leaks ~120uA through an enabled input buffer and
		// the internal pull-down is too weak to win.
		//
		{ 0x0a, 0b00000000 },	// GPIO1 disabled
		{ 0x1a, 0b01100000 },	// I2S protocol with 24-bit word length
		{ 0x1e, 0b00100000 },	// ADC channel 1 enabled, I2S left slot 0
		{ 0x1f, 0b00110000 },	// ADC channel 2 enabled, I2S right slot 0
		{ 0x28, 0b00100000 },	// DAC channel 1 enabled, I2S left slot 0
		{ 0x29, 0b00110000 },	// DAC channel 2 enabled, I2S right slot 0
		{ 0x4d, 0b00010100 },	// VREF set to 2.75V and MICBIAS set to VREF/2 with LDO GAIN 1.096 for 1V_{rms} single-ended input
		//
		// The input and output halves are the *board's* business
		// rather than the codec's, so they are the only part of
		// this that varies.  A board with capacitors into the
		// codec and a plain line output wants what it always
		// wanted; a DC-coupled board with a headphone jack wants
		// the common-mode tolerance widened, OUT1M driving the
		// jack's common and OUT2M sensing it at the far end.
		//
#ifdef CODEC_DC_COUPLED
		//
		// INSRC 01 single-ended and FULLSCALE_VAL 0 together are
		// what put 1Vrms at 0dBFS - the datasheet's "2 Vrms
		// differential (1 Vrms for single ended operation)" - and
		// that is the level the whole pedal is scaled around.
		//
		// CM_TOL 10 rather than 01: rail-to-rail costs a few dB of
		// noise where 500mVpp would probably do, and this silicon
		// is untested.  Take the tolerant one and measure.
		//
		{ 0x50, 0b01001000 },	// ADC Channel 1: single-ended DC-coupled input, 5kOhm, 1Vrms full scale, rail-to-rail common mode, audio bandwidth
		{ 0x55, 0b01001000 },	// ADC Channel 2: the same
		//
		// **These two go together and the jack is a short between
		// two drivers without them.**  OUT1x_CFG 101 makes OUT1P
		// the signal, OUT1M the common-mode driver and OUT2M a
		// sense input reading that common at the jack; OUT2x_CFG
		// below has to be 010 so channel 2 leaves OUT2M alone.
		// Both reset to 000, which is differential - and in
		// differential OUT2M drives the inverted right channel
		// into OUT1M, through the short at J303.  Which is why
		// 0x78 powering the outputs up is the last write here.
		//
		{ 0x64, 0b00110100 },	// Out 1 source is DAC signal chain, pseudo-differential with OUT1M as common and OUT2M sensing it
		{ 0x65, 0b01100010 },	// DAC OUT1P configured for headphone driver and audio bandwidth, Analog input is single-ended
		{ 0x66, 0b01100000 },	// DAC OUT1M configured for headphone driver - it carries both channels' return current
#else
		{ 0x50, 0b01000000 },	// ADC Channel 1 configured for AC-coupled single-ended input with 5kOhm input impedance and audio bandwidth
		{ 0x55, 0b01000000 },	// ADC Channel 2 configured for AC-coupled single-ended input with 5kOhm input impedance and audio bandwidth
		{ 0x64, 0b00101000 },	// Out 1 source is DAC signal chain, mono single-ended on OUT1P only
		{ 0x65, 0b00100010 },	// DAC OUT1P configured for line out driver and audio bandwidth, Analog input is single-ended
		{ 0x66, 0b00100000 },	// DAC OUT1M configured for line out driver and audio bandwidth, AIN1P impedance 4k4
#endif
		//
		// Source 001, the DAC signal chain, same as channel 1
		// above.  It used to be 010 - the analog bypass path -
		// against a comment that said DAC, on boards that never
		// routed the second channel and so could not show it.
		//
		{ 0x6b, 0b00101000 },	// Out 2 source is DAC signal chain, mono single-ended on OUT2P only, 0.6*Vref as common mode
#ifdef CODEC_DC_COUPLED
		{ 0x6c, 0b01100010 },	// DAC OUT2P configured for headphone driver and audio bandwidth, Analog input is single-ended
		{ 0x6d, 0b00100000 },	// OUT2M is the sense input in this mode and drives nothing, so its drive setting is left alone
#else
		{ 0x6c, 0b00100010 },	// DAC OUT2P configured for line out driver and audio bandwidth, Analog input is single-ended
		{ 0x6d, 0b00100000 },	// DAC OUT2M configured for line out driver and audio bandwidth, AIN2P impedance 4k4
#endif
		{ 0x72, 0b00011100 },	// Three biquads per ADC channel
		{ 0x73, 0b00011100 },	// Three biquads per DAC channel
		{ 0x76, 0b11001100 },	// Input Channel 1 and 2 enabled; Output Channel 1 and 2 enabled
		{ 0x78, 0b11100000 },	// ADC, DAC and MICBIAS Powered Up
	};
	tac5112_array_write(regwrite);
}
