//
// Basic TAC5112 setup code
//
#define TAC5112_ADDR	0x50

// Page and register address for biquad filter
// coefficients for the ADC record output channel 1
//
// NOTE! The 5112 could clone ADC channel 1 to
// channel 3, and then have a separate set of
// volume and three biquad filters for that
// channel too.
//
// Then we could mix them in using the ADC digital
// mixer coefficients (page 10), so we'd have two
// independent sets of three biquad filters on the
// input
//
#define BIQUAD_IN1 8,8
#define BIQUAD_IN2 8,88
#define BIQUAD_IN3 9,48

// Page and register address for biquad filter
// coefficients for the DAC output channel 1
//
// I think we could use the DAC mixer registers
// (on page 17) to mix in ADC loopback and
// signal generator data. Not the analog loopback
// I was looking for, but..
//
// The TAC5112 can do a whole lot more than I'm
// really using...
//
#define BIQUAD_OUT1 15,8
#define BIQUAD_OUT2 15,88
#define BIQUAD_OUT3 16,48

static int tac5112_read(void)
{
	char rxdata;
	return i2c_read_blocking(i2c_default, TAC5112_ADDR, &rxdata, 1, false);
}

static inline int tac5112_write(const unsigned char *data, int len)
{
	i2c_write_blocking(i2c_default, TAC5112_ADDR, data, len, false);
}

static int __tac5112_array_write(const unsigned char arr[][2], int nr)
{
	for (int i = 0; i < nr; i++)
		tac5112_write(arr[i], 2);
}
#define tac5112_array_write(arr) __tac5112_array_write(arr, ARRAY_SIZE(arr))

static void tac5112_set_page(int page)
{
	static int current = -1;
	if (page != current) {
		char bytes[2] = { 0, page };
		current = page;
		tac5112_write(bytes, 2);
	}
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
	// Do a dummy one-byte read to see if it's there
	if (tac5112_read() < 0)
		for (;;);

	static const unsigned char tac_reset[][2] = {
		{ 0x00, 0x00 },	// Page 0
		{ 0x01, 0x01 }, // SW reset
	};
	tac5112_array_write(tac_reset);

	// Wait for it to take effect
	sleep_ms(10);
	tac5112_read();

	static const unsigned char regwrite[][2] = {
		{ 0x00, 0b00000000 },	// Page 0
		{ 0x02, 0b00000001 },	// Exit Sleep Mode with DREG and VREF Enabled
		{ 0x1a, 0b01100000 },	// I2S protocol with 24-bit word length
		{ 0x4d, 0b00000000 },	// VREF and MICBIAS set to 2.75V for 1V_{rms} single-ended input
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
		{ 0x76, 0b10001000 },	// Input Channel 1 enabled; Output Channel 1 enabled
		{ 0x78, 0b11100000 },	// ADC, DAC and MICBIAS Powered Up
	};
	tac5112_array_write(regwrite);
}
