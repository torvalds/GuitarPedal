#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

// Core utility functions and helpers
#include "util.h"
#include "lfo.h"
#include "effect.h"

// Basic hardware initializations and PIO code
#include "board.h"
#include "tac5112.h"
#include "i2s.pio.h"
#include "biquad.h"

// Effects
#include "flanger.h"
#include "echo.h"
#include "fm.h"
#include "notch.h"

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

struct effect {
	void (*init)(float, float, float, float);
	float (*step)(float);
} effects[] = {
	{ notch_init, notch_step },
	{ flanger_init, flanger_step },
	{ echo_init, echo_step },
	{ fm_init, fm_step },
};

static struct lfo_state beep_state = {
	LFO_FREQ(330),
};

static void beep_init(float pot1, float pot2, float pot3, float pot4)
{
}

static float beep_step(float val)
{
	return lfo_step(&beep_state, lfo_sinewave) * 0.2;
}

struct effect beep_effect = { beep_init, beep_step };

// To avoid crackling, the low-level delay
// must not change abruptly when the pot is
// turned (or when the pot value just randomly
// fluctuates).
//
// So we take the requested delay as a target
// that we approach smoothly
#define UPDATE(x) x += 0.001 * (target_##x - x)

static inline void make_one_noise(PIO pio, uint tx, uint rx, struct effect *eff)
{
	for (int i = 0; i < 200; i++) {
		UPDATE(effect_delay);

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
			pwm_set_gpio_level(LED_PIN, PWM_50);
			continue;

		case 2:
			pwm_set_gpio_level(LED_PIN, PWM_100);
			enabled = 3;
			eff = &beep_effect;
			break;

		case 3:
			pwm_set_gpio_level(LED_PIN, PWM_50);
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
	pwm_config_set_wrap(&pwm, PWM_100);
	pwm_init(pwm_slice, &pwm, true);
	gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
	pwm_set_gpio_level(LED_PIN, PWM_50);
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
