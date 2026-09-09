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
