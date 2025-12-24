#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

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

static int tac5112_write(const unsigned char *data, int len)
{
	i2c_write_blocking(i2c_default, TAC5112_ADDR, data, len, false);
}

static void tac5112_init(void)
{
	int ret;
	char rxdata;

	static const unsigned char regwrite[][2] = {
		{ 0x02, 0x01 },	// DEV_MISC_CFG (2): Device not in sleep mode
		{ 0x64, 0x48 },	// OUT1x_CFG0: 01001000 output from analog bypass, mono single-ended OUT1P
		{ 0x65, 0x22 },	// OUT1x_CGF1: 00100010 300 Ohm output impedance, 0 dB level, 4.4k input impedance, single-ended input
		{ 0x66, 0x20 },	// OUT1x_CFG2: 00100000 300 Ohm OUT1M output, 0 dB, 4k4 input impedance
		{ 0x76, 0x88 },	// PWR_CFG 0x78: Input channel 1 and output 1 enabled
		{ 0x78, 0xe0 },	// PWR_CFG 0x78: 11100000: power up ADC DAC and MICBIAS
	};

	// Do a dummy one-byte read to see if it's there
	if (tac5112_read() < 0)
		for (;;);

	// Write 0 0 1 to the TAC5112: set PAGE_CFG 0, do a software reset
	tac5112_write("\0\0\001", 3);
	// Wait for it to take effect
	sleep_ms(10);
	tac5112_read();

	for (int i = 0; i < ARRAY_SIZE(regwrite); i++)
		tac5112_write(regwrite[i], 2);
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
	adc_set_round_robin(0xf);

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

	// This tests all four pots, the stomp switch, and the LED
	while (true) {
		int delay = 100;

		int level = enabled ? enabled > 1 ? 4095 : 4095 & ~adc_read() : 50;

		pwm_set_gpio_level(LED_PIN, level);
		sleep_ms(delay);
	}
}
