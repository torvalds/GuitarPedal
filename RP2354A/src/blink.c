#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

#include "i2s.pio.h"

#include "util.h"
#include "lfo.h"
#include "flanger.h"

// This is our delay-line, shared across all effects
float sample_array[SAMPLE_ARRAY_SIZE];
int sample_array_index;

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

static int64_t alarm_callback(alarm_id_t id, void *user_data)
{
	// It should have been cancelled?
	if (gpio_get(STOMP_PIN))
		return 0;

	enabled = 2;
	return 0;
}

// The stomp switch irq starts the watchdog on a falling edge
// We'll reboot for programming if you hold it for five seconds.
//
// Very short presses are ignored for debouncing.
// Shorter presses change the state.
// 1s+ press for effect switch
// 5s press for reboot.
#define WATCHDOG_TIMEOUT 5000
static void stomp_irq(uint gpio, uint32_t event_mask)
{
	static absolute_time_t last_time;
	static alarm_id_t alarm_id = -1;
	absolute_time_t now = get_absolute_time();

	watchdog_disable();
	cancel_alarm(alarm_id);

	// Stomp switch released
	if (gpio_get(STOMP_PIN)) {
		int64_t us_diff;

		if (is_nil_time(last_time))
			return;
		us_diff = absolute_time_diff_us(last_time, now);

		// Assume a _really_ short press was noise
		if (us_diff < 1000)
			return;

		// Long press should already have been dealt with
		if (us_diff > 1000000)
			return;

		enabled = !enabled;
		return;
	}

	// Stomp switch pressed. Add alarms
	last_time = now;
	alarm_id = add_alarm_in_ms(1000, alarm_callback, NULL, false);
	watchdog_enable(WATCHDOG_TIMEOUT, 1);
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
	tac5112_array_write(regwrite);
}

extern float flanger_step(float);

static struct lfo_state base_lfo, modulator_lfo;
static float base_freq, freq_range;

static void fm_init(float pot1, float pot2, float pot3, float pot4)
{
	base_freq = 20 + fastpow(10000.0, pot1);	// 20 .. 10kHz
	freq_range = pot2;
	set_lfo_freq(&modulator_lfo, 1 + 10*pot3);	// 1..11 Hz
}

static float fm_step(float in)
{
	float multiplier = 1 + lfo_step(&modulator_lfo) * freq_range;
	float freq = base_freq * multiplier;
	set_lfo_freq(&base_lfo, freq);
	return lfo_step(&base_lfo) * 0.3;
}

struct effect {
	void (*init)(float, float, float, float);
	float (*step)(float);
} effects[] = {
	{ flanger_init, flanger_step },
	{ delay_init, delay_step },
	{ fm_init, fm_step },
};

static struct lfo_state beep_state = {
	.type = lfo_sinewave,
	LFO_FREQ(330),
};

static void beep_init(float pot1, float pot2, float pot3, float pot4)
{
}

static float beep_step(float val)
{
	return lfo_step(&beep_state) * 0.2;
}

struct effect beep_effect = { beep_init, beep_step };

static inline float read_pot(void)
{
	return (4095 & ~adc_read()) * (1.0 / 4096);
}

static inline void make_one_noise(PIO pio, uint tx, uint rx, struct effect *eff)
{
	for (int i = 0; i < 200; i++) {
		int v = pio_sm_get_blocking(pio, rx) << 8;
		float val = v / (float) 0x80000000;

		val = eff->step(val);

		pio_sm_put_blocking(pio, tx, (int)(0x80000000 * val));
	}
}

static inline void make_noise(PIO pio, uint tx, uint rx)
{
	int current_effect = 0;
	struct effect *eff = effects;

	float pot1 = read_pot();
	float pot2 = read_pot();
	float pot3 = read_pot();
	float pot4 = read_pot();

	for (;;) {
		switch (enabled) {
		case 0:
			pwm_set_gpio_level(LED_PIN, 0);
			while (!enabled) {
				int v = pio_sm_get_blocking(pio, rx) << 8;
				pio_sm_put_blocking(pio, tx, v);
			}
			pwm_set_gpio_level(LED_PIN, 2048);
			continue;

		case 2:
			pwm_set_gpio_level(LED_PIN, 4095);
			enabled = 3;
			eff = &beep_effect;
			break;

		case 3:
			pwm_set_gpio_level(LED_PIN, 2048);
			enabled = 1;
			current_effect++;
			if (current_effect >= ARRAY_SIZE(effects))
				current_effect = 0;
			eff = effects+current_effect;
			break;

		default:
			break;
		}

		eff->init(pot1, pot2, pot3, pot4);
		make_one_noise(pio, tx, rx, eff);

		pot1 = read_pot();
		make_one_noise(pio, tx, rx, eff);

		pot2 = read_pot();
		make_one_noise(pio, tx, rx, eff);

		pot3 = read_pot();
		make_one_noise(pio, tx, rx, eff);

		pot4 = read_pot();
		make_one_noise(pio, tx, rx, eff);
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
	i2s_init();

	// This tests all four pots, the stomp switch, and the LED
	while (true) {
		int delay = 100;

		int level = enabled ? enabled > 1 ? 4095 : 4095 & ~adc_read() : 50;

		pwm_set_gpio_level(LED_PIN, level);
		sleep_ms(delay);
	}
}
