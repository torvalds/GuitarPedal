#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

#include "i2s.pio.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define STOMP_PIN	0
#define LED_PIN		1

#define I2C_SDA		4
#define I2C_SCL		5

#define I2S_BCLK	8
#define I2S_FSYNC	9
#define I2S_DIN		10
#define I2S_DOUT	11

// NOTE! The pots are reversed, so the 12-bit ADC read will read 4095 -> 0 clockwise
#define POT1		26
#define POT2		27
#define POT3		28
#define POT4		29

#define TAC5112_ADDR	0x50

volatile int enabled = 1;

// The stomp switch irq starts the watchdog on a falling edge
// We'll reboot for programming if you hold it for five seconds.
//
// Very short presses are ignored for debouncing.
// Shorter presses change the state.
// 1s+ press for long-press.
// 5s press for reboot.
#define WATCHDOG_TIMEOUT 2000
void stomp_irq(uint gpio, uint32_t event_mask)
{
	int val;

	if (event_mask & GPIO_IRQ_EDGE_FALL) {
		watchdog_enable(WATCHDOG_TIMEOUT, 1);
		return;
	}

	val = WATCHDOG_TIMEOUT - watchdog_get_time_remaining_ms();
	watchdog_disable();

	// Arbitrary 5ms debounce time
	if (val < 5)
		return;

	// Regular short-press?
	if (val < 1000) {
		enabled = enabled != 1;
		return;
	}

	// Do something more interesting for long-press?
	enabled = 2;
}

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
		{ 0x76, 0b10001000 },	// Input Channel 1 enabled; Output Channel 1 enabled
		{ 0x78, 0b11100000 },	// ADC, DAC and MICBIAS Powered Up
	};
	// Apply FSYNC = 48 kHz and BCLK = 12.288 MHz and
	// Start recording/playback data by host on ASI bus with TDM protocol 32-bits channel wordlength
	tac5112_array_write(regwrite);
}

#define STEPS (48000 / 480)

static inline void make_noise(PIO pio, uint tx, uint rx)
{
	static int array[STEPS];

	for (int i = 0 ; i < STEPS; i++)
		array[i] = 0x7fffffff * sin(i * 6.2831853/STEPS);

	// Instead of blinking the LED, we just test DOUT
	// with a 220Hz triangle wave
	for (;;) {
		uint pot1 = 4095 & ~adc_read();
		float multiplier = pot1 / 4096.0;
		for (int i = 0; i < STEPS; i++) {
			int val;
			if (pio_sm_get_rx_fifo_level(pio, rx))
				val = pio_sm_get(pio, rx) << 8;
			else
				val = array[i];
			pio_sm_put_blocking(pio, tx, (int)(multiplier * val));
		}
	}
}

// Start our dummy "i2s" program
static void i2s_init(void)
{
	PIO pio = pio0;
	uint tx_offset, rx_offset;

	tx_offset = pio_add_program(pio, &i2s_tx_program);
	rx_offset = pio_add_program(pio, &i2s_rx_program);

	i2s_tx_program_init(pio, 0, tx_offset, I2S_BCLK);
	i2s_rx_program_init(pio, 1, rx_offset, I2S_BCLK);

	// Do something
	make_noise(pio, 0, 1);
}


int main()
{
	watchdog_reboot(0, 0, WATCHDOG_TIMEOUT);
	watchdog_start_tick(12);
	watchdog_disable();

	// STOMP_PIN is a plain input GPIO, pulled down
	// by by the momentary stomp switch. Maybe do it
	// as an interrupt source eventually, but for
	// now just read it with 'gpio_get()'.
	gpio_init(STOMP_PIN);
	gpio_set_dir(STOMP_PIN, GPIO_IN);
	gpio_pull_up(STOMP_PIN);
	gpio_set_irq_enabled_with_callback(STOMP_PIN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, stomp_irq);

	// The LED is done as a plain GPIO output. I
	// think I want to try to just PWM it, but
	// that's for later.
	pwm_config pwm = pwm_get_default_config();
	int pwm_slice = pwm_gpio_to_slice_num(LED_PIN);
	pwm_config_set_wrap(&pwm, 4096);
	pwm_init(pwm_slice, &pwm, true);
	gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
	pwm_set_gpio_level(LED_PIN, 2048);
	pwm_set_enabled(pwm_slice, true);

	// We use all four ADC's, set up as round
	// robin. Use mask 0xf to ignore the onboard
	// temperature sensor.
	adc_init();
	adc_gpio_init(POT1);
	adc_select_input(0);
//	adc_set_round_robin(0xf);

	// The TAC5112 is programmed over i2c, connected
	// to pins 4 (SDA) and 5 (SCL). There are pull-ups
	// on the board, but we'll do them here too.
	//
	// The TAC5112 can do 100/400/1000 kHz i2c, but
	// we really don't care. So use 100kHz standard
	// mode.
	i2c_init(i2c_default, 100 * 1000);
	gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
	gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
	gpio_pull_up(I2C_SDA);
	gpio_pull_up(I2C_SCL);

	tac5112_init();
	i2s_init();

	// This tests all four pots, the stomp switch, and the LED
	while (true) {
		int delay = 100;

		int level = enabled ? enabled > 1 ? 4095 : 4095 & ~adc_read() : 50;

		pwm_set_gpio_level(LED_PIN, level);
		sleep_ms(delay);
	}
}
