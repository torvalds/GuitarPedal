#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "board.h"

#include "usb-sync.h"

#include "ws2812.pio.h"
#include "debounce.pio.h"
#include "rotary.pio.h"
#include "i2s.pio.h"

#define PIO0_I2S_TX_SM 0
#define PIO0_I2S_RX_SM 1
#define PIO0_WS2812_SM 2

#include "audio/util.h"

#include "tusb.h"
#include "usb-audio.h"
#include "sh1106.h"
#include "switch.h"

static volatile int next_state_seq = 1;
static const struct effect *last_effect = NULL;

#include "audio/tac5112.h"
#include "audio/effect.h"

static void reset_effect(struct effect *eff)
{
	eff->active_pot = 0;
	eff->last = -1;
	eff->seq = 0;
	eff->mix = eff->target = 0;
	for (int i = 0; i < 10; i++) {
		signed char def_val = eff->pots[i].def_val;
		eff->pot_values[0][i] = def_val;
		eff->pot_values[1][i] = def_val;
	}
}

static void init_i2s(void)
{
	uint tx_offset, rx_offset;

	tx_offset = pio_add_program(pio0, &i2s_tx_program);
	rx_offset = pio_add_program(pio0, &i2s_rx_program);

	i2s_tx_program_init(pio0, PIO0_I2S_TX_SM, tx_offset, I2S_BCLK);
	i2s_rx_program_init(pio0, PIO0_I2S_RX_SM, rx_offset, I2S_BCLK);
}

static void init_ws2812(void)
{
#ifdef WS2812_PIN
	uint offset = pio_add_program(pio0, &ws2812_program);
	ws2812_program_init(pio0, PIO0_WS2812_SM, offset, WS2812_PIN);
#endif
}

// Initialize a pin for input, pulled up
static void init_sw_pin(PIO pio, int pin)
{
	gpio_init(pin);
	gpio_set_dir(pin, false);
	gpio_pull_up(pin);
	pio_gpio_init(pio, pin);
}

// I have no good way to detect USB when in USB host mode.
//
// In a perfect world, I would have a GPIO that would tell
// me whether the power is provided by the 9V guitar power
// supply or the USB line, but ...
static inline bool usb_is_connected(void)
{
#ifdef USB_MODE_HOST
	return true;
#else
	return tud_ready();
#endif
}

// We use PIO1 for the SW pins.
//
// They share the same program, just a separate state machine
// for each pin.
static void switch_irq(void)
{
	PIO pio = pio1;

	for (int idx = 0; idx < 4; idx++) {
		if (pio_sm_is_rx_fifo_empty(pio, idx))
			continue;

		int bit = idx + 16*!!pio_sm_get(pio, idx);
		switch_val |= 1 << bit;
	}

	// Long-press with *both* SW1/2 pressed down
	// turns it into programming mode, but only when
	// it's already connected to USB
	//
	// Otherwise it resets all effects to their
	// default pot values
	if (switch_val & 0x30000) {
		if (!gpio_get(GPIO_SW1) && !gpio_get(GPIO_SW2)) {
			if (usb_is_connected())
				reset_usb_boot(0, 0);
			last_effect = NULL;
			for (int i = 0; i < ARRAY_SIZE(effects); i++) {
				struct effect *eff = effects[i];
				reset_effect(eff);
			}
		}
	}
}

volatile bool ui_sync_changed = false;

#ifdef USB_MODE_HOST
uint8_t remote_clipping = 0;
uint8_t remote_intense = 0;
uint8_t remote_dropped = 0;
#endif

int current_midi_effect_idx = 0;

void handle_midi_packet(const uint8_t packet[4])
{
	uint8_t status = packet[1];
	uint8_t data1 = packet[2];
	uint8_t data2 = packet[3];

	if ((status & 0xF0) == 0xB0) {
		// Control Change
		if (data1 >= MIDI_CC_POT_START && data1 < MIDI_CC_POT_START + 10) {
			int pot_idx = data1 - MIDI_CC_POT_START;
			if (current_midi_effect_idx < ARRAY_SIZE(effects) && data2 > 0) {
				struct effect *effect = effects[current_midi_effect_idx];
				int val = data2 - 64;
				if (val < -60) val = -60;
				if (val > 60) val = 60;
				effect->pot_values[0][pot_idx] = val;
				effect->pot_values[1][pot_idx] = val;
				effect->seq++;
				ui_sync_changed = true;
			}
		} else if (data1 == MIDI_CC_GLOBAL_ENABLE) {
			disable_all = (data2 == 0) ? EFF_ENABLE_STEPS : 0;
			ui_sync_changed = true;
		} else if (data1 == MIDI_CC_EFFECT_ENABLE) {
			if (current_midi_effect_idx < ARRAY_SIZE(effects)) {
				struct effect *effect = effects[current_midi_effect_idx];
				effect->target = (data2 == 0) ? 0 : EFF_ENABLE_STEPS;
				effect->seq++;
				ui_sync_changed = true;
			}
		} else if (data1 == MIDI_CC_ACTIVE_POT) {
			if (current_midi_effect_idx < ARRAY_SIZE(effects)) {
				effects[current_midi_effect_idx]->active_pot = data2;
				ui_sync_changed = true;
			}
		} else if (data1 == MIDI_CC_AUDIO_CLIPPING) {
#ifdef USB_MODE_HOST
			remote_clipping = (data2 > 0);
#endif
		} else if (data1 == MIDI_CC_EFFECT_INTENSE) {
#ifdef USB_MODE_HOST
			remote_intense = (data2 > 0);
#endif
		} else if (data1 == MIDI_CC_CPU_LATENCY) {
#ifdef USB_MODE_HOST
			remote_dropped = data2;
#endif
		}
	} else if ((status & 0xF0) == 0xC0) {
		// Program Change
		if (data1 < ARRAY_SIZE(effects)) {
			current_midi_effect_idx = data1;
			ui_sync_changed = true;

#ifndef USB_MODE_HOST
			struct effect *effect = effects[current_midi_effect_idx];
			send_midi_cc(MIDI_CC_EFFECT_ENABLE, effect->target ? 127 : 0);
			send_midi_cc(MIDI_CC_ACTIVE_POT, effect->active_pot);
			for (int i=0; i<10; i++) {
				int val = effect->pot_values[0][i];
				uint8_t midi_val = val + 64;
				send_midi_cc(MIDI_CC_POT_START + i, midi_val);
			}
#endif
		}
	}
}

static void init_sw_pins(void)
{
	PIO pio = pio1;
	uint offset = pio_add_program(pio, &debounce_program);

	init_sw_pin(pio, GPIO_SW1);
	init_sw_pin(pio, GPIO_SW2);
	init_sw_pin(pio, GPIO_SW3);
	init_sw_pin(pio, GPIO_SW4);

	// We use the same PIO program for both SW pins
	// just with different state machines
	debounce_program_init(pio, 0, offset, GPIO_SW1);
	debounce_program_init(pio, 1, offset, GPIO_SW2);
	debounce_program_init(pio, 2, offset, GPIO_SW3);
	debounce_program_init(pio, 3, offset, GPIO_SW4);

	irq_set_exclusive_handler(PIO1_IRQ_0, switch_irq);
	irq_set_enabled(PIO1_IRQ_0, true);
}

#define PWM_100 4096
#define PWM_50 2048

static void init_one_pwm_pin(int pin)
{
	unsigned int slice = pwm_gpio_to_slice_num(pin);

	gpio_set_function(pin, GPIO_FUNC_PWM);
	pwm_set_wrap(slice, PWM_100);
	pwm_set_gpio_level(pin, 0);
	pwm_set_enabled(slice, true);
}

static void init_pwm_pins(void)
{
	init_one_pwm_pin(PWM_PIN1);
	init_one_pwm_pin(PWM_PIN2);
	pwm_set_gpio_level(PWM_PIN1, 0);
	pwm_set_gpio_level(PWM_PIN2, 0);
}

static void init_i2c_bus(i2c_inst_t *i2c, int kbps, int sda, int scl)
{
	i2c_init(i2c, kbps * 1000);
	gpio_set_function(sda, GPIO_FUNC_I2C);
	gpio_set_function(scl, GPIO_FUNC_I2C);
	gpio_pull_up(sda);
	gpio_pull_up(scl);
}

// Top rotary for values:
//  - rotate to change
//  - press to switch to next value
//  - hold and rotate to move values
#define rotary_value (rotary_array[0])
#define rotary_select (rotary_array[2])

// Bottom rotary for effects:
//  - rotate to change
//  - press to enable/disable
//  - hold and rotate for what?
#define rotary_effect (rotary_array[1])
#define rotary_what (rotary_array[3])

static volatile int rotary_array[4];

static void rotary_irq(void)
{
	// Initial impossible previous values
	static int prev_value[2] = { 4, 4 };
	static const int lookup[32] = {
		// CW: 00 -> 10 -> 11 -> 01 -> 00
		[2] = 1, [11] = 1, [13] = 1, [4] = 1,
		// CCW: 00 -> 01 -> 11 -> 10 -> 00
		[1] = -1, [7] = -1, [14] = -1, [8] = -1
	};

	for (int sm = 0; sm < 2; ) {
		if (pio_sm_is_rx_fifo_empty(pio2, sm)) {
			sm++;
			continue;
		}
		int curr = pio_sm_get(pio2, sm) & 3;
		int prev = prev_value[sm];

		int val = lookup[(prev << 2) | curr];
		prev_value[sm] = curr;

		if (!val)
			continue;

		// Is the switch pressed?
		int gpio = sm ? GPIO_SW2 : GPIO_SW1;
		int rotary_idx = gpio_get(gpio) ? sm : sm+2;

		rotary_array[rotary_idx] += val;
	}
}

// We'll use a separate PIO program for the rotary
// encoder pins eventually
static void init_rotary_encoder(void)
{
	PIO pio = pio2;
	uint offset = pio_add_program(pio, &rotary_program);

	init_sw_pin(pio, GPIO_ROT1A);
	init_sw_pin(pio, GPIO_ROT1B);
	rotary_program_init(pio, 0, offset, GPIO_ROT1A);

	init_sw_pin(pio, GPIO_ROT2A);
	init_sw_pin(pio, GPIO_ROT2B);
	rotary_program_init(pio, 1, offset, GPIO_ROT2A);

	irq_set_exclusive_handler(PIO2_IRQ_0, rotary_irq);
	irq_set_enabled(PIO2_IRQ_0, true);
}

#include "ui.h"

static void audio_processing(void)
{
	// Get everything going, then clear 'dropped'
	// to get rid of any initialization issues
	make_one_noise();
	for (int i=0; i<8; i++)
		pio_sm_put_blocking(pio0, PIO0_I2S_TX_SM, 0);
	dropped = 0;

	for (;;) {
		make_one_noise();
	}
}

unsigned get_audio_samples(int32_t *buffer, unsigned nr)
{
	return get_output_samples((s32 *)buffer, nr);
}

#include "eeprom.h"

static void init_effects(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];

		reset_effect(effect);
		load_effect_state(i, effect);
		effect->init(effect->pot_values[0]);
	}
}

int main()
{
	absolute_time_t now;
	absolute_time_t next_ui_update;

	set_sys_clock_khz(172800, true);

	init_i2s();
	init_ws2812();
	init_sw_pins();
	init_pwm_pins();
	init_rotary_encoder();
	init_i2c_bus(i2c0, 400, I2C0_SDA, I2C0_SCL);
	init_i2c_bus(i2c1, 400, I2C1_SDA, I2C1_SCL);

	init_usb();

	now = get_absolute_time();

	// Power-up delay for sh1106
	next_ui_update = delayed_by_ms(now, 500);

	tac5112_init();

	init_effects();

	multicore_launch_core1(audio_processing);

	for (;;) {
		absolute_time_t now = get_absolute_time();

#ifdef USB_MODE_HOST
		tuh_task();
#else
		tud_task();
		usb_audio_task();
#endif
		sh1106_task();

		// Claim 25Hz screen updates
		if (now > next_ui_update) {
			next_ui_update = delayed_by_ms(now, 40);
			update_ui(to_ms_since_boot(now));

#ifndef USB_MODE_HOST
			unsigned int current_dropped = __atomic_exchange_n(&dropped, 0, __ATOMIC_RELAXED);
			if (current_dropped) {
				int midi_dropped = current_dropped;
				if (midi_dropped > 127) midi_dropped = 127;
				send_midi_cc(MIDI_CC_CPU_LATENCY, midi_dropped);
			}
#endif
		}
	}
}
