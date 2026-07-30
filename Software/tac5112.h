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
		{ 0x1a, 0b01100000 },	// I2S protocol with 24-bit word length
		{ 0x4d, 0b00010100 },	// VREF set to 2.75V and MICBIAS set to VREF/2 with LDO GAIN 1.096 for 1V_{rms} single-ended input
		{ 0x50, 0b01000000 },	// ADC Channel 1 configured for AC-coupled single-ended input with 5kOhm input impedance and audio bandwidth
		{ 0x55, 0b01000000 },	// ADC Channel 2 configured for AC-coupled single-ended input with 5kOhm input impedance and audio bandwidth
		{ 0x64, 0b00101000 },	// Out 1 source is DAC signal chain, mono single-ended on OUT1P only
		{ 0x65, 0b00100010 },	// DAC OUT1P configured for line out driver and audio bandwidth, Analog input is single-ended
		{ 0x66, 0b00100000 },	// DAC OUT1M configured for line out driver and audio bandwidth, AIN1P impedance 4k4
		{ 0x6b, 0b01001000 },	// DAC Channel 2 configured for AC-coupled single-ended mono output with 0.6*Vref as common mode
		{ 0x6c, 0b00100010 },	// DAC OUT2P configured for line out driver and audio bandwidth, Analog input is single-ended
		{ 0x6d, 0b00100000 },	// DAC OUT2M configured for line out driver and audio bandwidth, AIN2P impedance 4k4
		{ 0x72, 0b00011100 },	// Three biquads per ADC channel
		{ 0x73, 0b00011100 },	// Three biquads per DAC channel
		{ 0x76, 0b11001100 },	// Input Channel 1 and 2 enabled; Output Channel 1 and 2 enabled
		{ 0x78, 0b11100000 },	// ADC, DAC and MICBIAS Powered Up
	};
	tac5112_array_write(regwrite);
}
