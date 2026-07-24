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
#include "hardware/dma.h"

#include "board.h"

#include "ws2812.pio.h"
#include "debounce.pio.h"
#include "rotary.pio.h"
#include "i2s.pio.h"

#define PIO0_I2S_TX_SM 0
#define PIO0_I2S_RX_SM 1
#define PIO0_WS2812_SM 2

#define PWM_WRAP 4096	// Entirely arbitrary

#include "audio/types.h"
#include "audio/util.h"
#include "audio/envelope.h"
#include "audio/single-pole.h"
#include "audio/biquad.h"
#include "audio/fft.h"
#include "audio/analyze.h"

#include "midi.h"
#include "uart.h"
#include "tusb.h"
#include "usb-audio.h"
#include "sh1106.h"
#include "switch.h"

static int tuner_mode = 0;
static volatile int user_interaction = 0;
static volatile int next_state_seq = 1;
static const struct effect *last_effect = NULL;

#include "audio/tac5112.h"
#include "audio/effect.h"

uint8_t effect_chain[15];
uint8_t effect_chain_len = 0;
uint8_t routed_effect_count = 0;

#include "eeprom.h"

static void reset_effect(struct effect *eff)
{
	eff->active_pot = 0;
	eff->last = -1;
	eff->seq = 0;
	eff->mix = eff->target = 0;
	eff->mix_pot = (unsigned int)(eff->def_mix * EFF_ENABLE_STEPS);
	for (int i = 0; i < 10; i++) {
		unsigned char def_val = eff->pots[i].def_val;
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

	dma_rx = dma_claim_unused_channel(true);
	dma_channel_config c_rx = dma_channel_get_default_config(dma_rx);
	channel_config_set_transfer_data_size(&c_rx, DMA_SIZE_32);
	channel_config_set_read_increment(&c_rx, false);
	channel_config_set_write_increment(&c_rx, true);
	channel_config_set_dreq(&c_rx, pio_get_dreq(pio0, PIO0_I2S_RX_SM, false));
	channel_config_set_ring(&c_rx, true, 7); // write wrap at 128 bytes (32 words)

	dma_tx = dma_claim_unused_channel(true);
	dma_channel_config c_tx = dma_channel_get_default_config(dma_tx);
	channel_config_set_transfer_data_size(&c_tx, DMA_SIZE_32);
	channel_config_set_read_increment(&c_tx, true);
	channel_config_set_write_increment(&c_tx, false);
	channel_config_set_dreq(&c_tx, pio_get_dreq(pio0, PIO0_I2S_TX_SM, true));
	channel_config_set_ring(&c_tx, false, 7); // read wrap at 128 bytes (32 words)

	pio_sm_clear_fifos(pio0, PIO0_I2S_RX_SM);
	pio_sm_clear_fifos(pio0, PIO0_I2S_TX_SM);

	// RX and TX start at the same point, together. But TX will
	// fill up the PIO buffers and move ahead, while RX will be
	// waiting for the first samples to come in, so it naturally
	// falls behind.
	//
	// And "falls behind" is the same as "is ahead" in a circular
	// buffer.
	dma_channel_configure(dma_rx, &c_rx, i2s_dma_buf, &pio0->rxf[PIO0_I2S_RX_SM], 0xffffffff, false);
	dma_channel_configure(dma_tx, &c_tx, &pio0->txf[PIO0_I2S_TX_SM], i2s_dma_buf, 0xffffffff, false);

	dma_start_channel_mask((1u << dma_rx) | (1u << dma_tx));
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
	user_interaction = 1;
}

volatile bool ui_sync_changed = false;

#ifdef USB_MODE_HOST
uint8_t remote_clipping = 0;
uint8_t remote_intense = 0;
uint8_t remote_dropped = 0;
#endif

static bool remote_tuner_data = false;
static uint8_t remote_note_idx[9] = {0};
static int8_t remote_cents[9] = {0};

int current_midi_effect_idx = 0;

#include "midi_schema.h"

static int schema_tx_index = -1;

static void send_sysex_schema_chunk(void)
{
	if (schema_tx_index < 0) return;

	int len = strlen(midi_schema_json);
	while (schema_tx_index <= len + 1) {
		uint8_t packet[4];
		int next_idx = schema_tx_index;

		if (schema_tx_index == 0) {
			packet[0] = 0x04; packet[1] = 0xF0; packet[2] = 0x7D; packet[3] = 0x02;
			next_idx = 1;
		} else {
			int i = schema_tx_index - 1;
			int remain = len - i;
			if (remain >= 3) {
				packet[0] = 0x04;
				packet[1] = midi_schema_json[i++];
				packet[2] = midi_schema_json[i++];
				packet[3] = midi_schema_json[i++];
				next_idx = i + 1;
			} else if (remain == 2) {
				packet[0] = 0x07;
				packet[1] = midi_schema_json[i++];
				packet[2] = midi_schema_json[i++];
				packet[3] = 0xF7;
				next_idx = len + 2;
			} else if (remain == 1) {
				packet[0] = 0x06;
				packet[1] = midi_schema_json[i++];
				packet[2] = 0xF7;
				packet[3] = 0;
				next_idx = len + 2;
			} else {
				packet[0] = 0x05;
				packet[1] = 0xF7;
				packet[2] = 0;
				packet[3] = 0;
				next_idx = len + 2;
			}
		}

		if (!tud_midi_packet_write(packet)) break; // FIFO full
		schema_tx_index = next_idx;

		if (schema_tx_index >= len + 2) {
			schema_tx_index = -1;
			break;
		}
	}
}

static int state_dump_eff_idx = -1;
static int state_dump_pot_idx = 0;
static int state_dump_packet_idx = 0;
static int state_dump_routing_idx = -1;

static void send_state_dump_chunk(void)
{
	if (state_dump_eff_idx < 0) return;

	while (state_dump_eff_idx < ARRAY_SIZE(effects)) {
		if (state_dump_eff_idx == 0 && state_dump_pot_idx == 0) {
			uint8_t packet[4] = { 0x0B, 0xB0, MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127 };
			if (!tud_midi_packet_write(packet))
				return;
			state_dump_pot_idx = 1;
		} else {
			struct effect *e = effects[state_dump_eff_idx];
			uint8_t packet[4];

			if (state_dump_packet_idx == 0) {
				packet[0] = 0x04; packet[1] = 0xF0; packet[2] = 0x7D; packet[3] = 0x03;
			} else if (state_dump_packet_idx == 1) {
				packet[0] = 0x04; packet[1] = state_dump_eff_idx;
				if (state_dump_pot_idx == 1) {
					packet[2] = 0; packet[3] = (e->mix_pot * 120) / EFF_ENABLE_STEPS;
				} else {
					packet[2] = state_dump_pot_idx - 1;
					packet[3] = e->pot_values[e->seq & 1][state_dump_pot_idx - 2];
				}
			} else {
				packet[0] = 0x05; packet[1] = 0xF7; packet[2] = 0; packet[3] = 0;
			}

			if (!tud_midi_packet_write(packet))
				return;

			state_dump_packet_idx++;
			if (state_dump_packet_idx == 3) {
				state_dump_packet_idx = 0;
				state_dump_pot_idx++;
				if (state_dump_pot_idx == 12) {
					state_dump_pot_idx = 1;
					state_dump_eff_idx++;
				}
			}
		}
	}

	if (state_dump_routing_idx == -1) {
		uint8_t packet[4] = { 0x04, 0xF0, 0x7D, 0x08 };
		if (!tud_midi_packet_write(packet))
			return;
		state_dump_routing_idx = 0;
	}

	while (state_dump_routing_idx < routed_effect_count) {
		int remaining = routed_effect_count - state_dump_routing_idx;
		uint8_t packet[4];
		if (remaining >= 3) {
			packet[0] = 0x04; // 3 bytes
			packet[1] = effect_chain[state_dump_routing_idx++];
			packet[2] = effect_chain[state_dump_routing_idx++];
			packet[3] = effect_chain[state_dump_routing_idx++];
		} else if (remaining == 2) {
			packet[0] = 0x07; // 3 bytes, ends with F7
			packet[1] = effect_chain[state_dump_routing_idx++];
			packet[2] = effect_chain[state_dump_routing_idx++];
			packet[3] = 0xF7;
		} else if (remaining == 1) {
			packet[0] = 0x06; // 2 bytes, ends with F7
			packet[1] = effect_chain[state_dump_routing_idx++];
			packet[2] = 0xF7;
			packet[3] = 0;
		}
		if (!tud_midi_packet_write(packet)) {
			state_dump_routing_idx -= (remaining >= 3) ? 3 : remaining;
			return;
		}

		if (remaining <= 2) {
			// Finished!
			state_dump_eff_idx = -1;
			return;
		}
	}

	// If routed_effect_count was a multiple of 3, we need to send just F7
	if (state_dump_routing_idx == routed_effect_count) {
		uint8_t packet[4] = { 0x05, 0xF7, 0, 0 }; // 1 byte (F7)
		if (!tud_midi_packet_write(packet))
			return;
		state_dump_eff_idx = -1;
	}
}

static uint8_t sysex_buf[32];
static int sysex_len = 0;
static bool in_sysex = false;

static void handle_sysex_payload(uint8_t *sysex_buf, size_t sysex_len)
{
	uint8_t cmd = sysex_buf[0];
	if (cmd == 0x01) { // Schema Request
		schema_tx_index = 0;
	} else if (cmd == 0x03 && sysex_len >= 4) { // Set Parameter
		uint8_t eff_id = sysex_buf[1];
		uint8_t pot_idx = sysex_buf[2];
		uint8_t val = sysex_buf[3];
		struct effect *e = NULL;
		if (eff_id < ARRAY_SIZE(effects)) {
			e = effects[eff_id];
		}
		if (e) {
			if (pot_idx == 0) {
				e->mix_pot = (val * EFF_ENABLE_STEPS) / 120;

				bool routed = (e == effects[0] || e == effects[EFFECT_COUNT - 1]);
				for (int i = 0; !routed && i < effect_chain_len; i++) {
					if (effects[effect_chain[i]] == e) routed = true;
				}
				e->target = routed ? e->mix_pot : 0;
			} else if (pot_idx <= 10) {
				unsigned int seq = e->seq;
				unsigned char *cur_pot = e->pot_values[seq & 1];
				unsigned char *new_pot = e->pot_values[!(seq & 1)];
				memcpy(new_pot, cur_pot, 10);
				new_pot[pot_idx - 1] = val;
				e->seq++;
			}
			ui_sync_changed = true;
		}
	} else if (cmd == 0x04 && sysex_len >= 2) { // Save Scene
		uint8_t scene_id = sysex_buf[1];
		save_scene(scene_id);

	} else if (cmd == 0x05) { // State Dump Request
		state_dump_eff_idx = 0;
		state_dump_pot_idx = 0;
		state_dump_packet_idx = 0;
		state_dump_routing_idx = -1;
	} else if (cmd == 0x08) { // Set Routing Order
		routed_effect_count = 0;

		// Routing order
		for (int i = 1; i < sysex_len && routed_effect_count < 14; i++) {
			uint8_t eff_id = sysex_buf[i];
			effect_chain[routed_effect_count++] = eff_id;
			if (eff_id < EFFECT_COUNT) {
				effects[eff_id]->target = effects[eff_id]->mix_pot;
			}
		}

		// Append unrouted effects and set target to 0
		effect_chain_len = routed_effect_count;
		for (int i = 1; i < EFFECT_COUNT - 1; i++) {
			bool routed = false;
			for (int j = 0; j < routed_effect_count; j++) {
				if (effect_chain[j] == i) { routed = true; break; }
			}
			if (!routed) {
				effect_chain[effect_chain_len++] = i;
				effects[i]->target = 0;
			}
		}
		ui_sync_changed = true;
	}
}


bool handle_midi_packet(const uint8_t packet[4])
{
	uint8_t code = packet[0] & 0x0F;

	// Handle SysEx parsing across packets
	if (code == 0x04 || code == 0x05 || code == 0x06 || code == 0x07) {
		for (int i = 1; i <= 3; i++) {
			uint8_t b = packet[i];
			if (b == 0xF0) {
				in_sysex = true;
				sysex_len = 0;
			} else if (b == 0xF7 && in_sysex) {
				in_sysex = false;
				handle_sysex_payload(sysex_buf, sysex_len);
			} else if (in_sysex) {
				if (sysex_len == 0 && b == 0x7D) {
					// Consume header 7D
				} else if (sysex_len < sizeof(sysex_buf)) {
					sysex_buf[sysex_len++] = b;
				}
			}
			if (code == 0x05 && i == 1) break;
			if (code == 0x06 && i == 2) break;
		}
		return true;
	}

	uint8_t status = packet[1];
	uint8_t data1 = packet[2];
	uint8_t data2 = packet[3];

	if (settings.midi_channel != 0) {
		if ((status & 0x0F) != (settings.midi_channel - 1))
			return false;
	}

	bool handled = false;

	if ((status & 0xF0) == 0xB0) {
		handled = true;
		if (data1 == 20) { // Global Bypass
			if (data2 == 68) {
				tuner_mode = 1;
			} else if (data2 == 69) {
				tuner_mode = 0;
			} else if (data2 == 126) {
				reset_usb_boot(0, 0);
			} else {
				disable_all = (data2 == 0) ? EFF_ENABLE_STEPS : 0;
			}
			ui_sync_changed = true;
		} else if (data1 == 7) { // Volume
			// Not yet wired globally
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
	} else if ((status & 0xF0) == 0x90) { // Note On
		handled = true;
		uint8_t ch = status & 0x0F;
		if (ch < 9) {
			remote_note_idx[ch] = data1;
			remote_tuner_data = true;
		}
	} else if ((status & 0xF0) == 0x80) { // Note Off
		handled = true;
		uint8_t ch = status & 0x0F;
		if (ch < 9 && remote_note_idx[ch] == data1) {
			remote_note_idx[ch] = 0;
			remote_tuner_data = true;
		}
	} else if ((status & 0xF0) == 0xE0) { // Pitch Bend
		handled = true;
		uint8_t ch = status & 0x0F;
		if (ch < 9) {
			uint16_t bend = data1 | (data2 << 7);
			int cents = ((int)bend - 8192) / 41;
			remote_cents[ch] = (int8_t)cents;
			remote_tuner_data = true;
		}
	} else if ((status & 0xF0) == 0xC0) {
		handled = true;
		// Program Change -> Load Scene
		if (data1 < MAX_SCENES) {
			load_scene(data1);
			ui_sync_changed = true;
		}
	}

	return handled;
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

static void init_one_pwm_pin(int pin)
{
	unsigned int slice = pwm_gpio_to_slice_num(pin);

	gpio_set_function(pin, GPIO_FUNC_PWM);
	pwm_set_wrap(slice, PWM_WRAP);
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
	user_interaction = 1;
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

#if !MIDI_HW
	init_sw_pin(pio, GPIO_ROT2A);
	init_sw_pin(pio, GPIO_ROT2B);
	rotary_program_init(pio, 1, offset, GPIO_ROT2A);
#endif

	irq_set_exclusive_handler(PIO2_IRQ_0, rotary_irq);
	irq_set_enabled(PIO2_IRQ_0, true);
}

#include "ui.h"

static inline void enable_ftz(void)
{
	// FZ bit (24) in FPSCR flushes subnormal results to zero in hardware,
	// covering every float op in the audio chain. Without it, any feedback
	// path that decays into sub-1e-38 range causes a 5-20x FPU slowdown on
	// Cortex-M33 (VFPv5 handles subnormals in hardware, not via trap, but
	// still at a significant penalty). Must be set per-core.
	uint32_t fpscr;

	fpscr = __builtin_arm_get_fpscr();
	fpscr |= 1u << 24;
	__builtin_arm_set_fpscr(fpscr);
}

static void audio_processing(void)
{
	enable_ftz();
	for (;;)
		make_one_noise();
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
	}

	if (!load_scene(0)) {
		// Default chain if EEPROM is empty
		routed_effect_count = 0;
		effect_chain_len = 0;
		extern struct effect gate_effect;
		extern struct effect settings_effect;
	}

	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		effect->init(effect->pot_values[0]);
	}
}

#include "tuner_ui.h"

static void tuner_mode_ui(void)
{
	// UI update at 25Hz -> ~10s cycle -> 5s half-cycle of sin^2
	static struct lfo_state breathe = { .step = 0x01000000 };

	float led = lfo_step(&breathe, lfo_sinewave);
	led = led*led;

	int level = led_pwm_mapping(led * settings.led_pwm);
	pwm_set_gpio_level(PWM_PIN1, level);
	level = led_pwm_mapping((1-led) * settings.led_pwm);
	pwm_set_gpio_level(PWM_PIN2, level);

	draw_analyzer();
}

int main()
{
	int forced_update = 1;

	enable_ftz();

	init_i2s();
	init_ws2812();
	init_sw_pins();
	init_pwm_pins();
	init_rotary_encoder();
	init_i2c_bus(i2c0, 400, I2C0_SDA, I2C0_SCL);
	init_i2c_bus(i2c1, 400, I2C1_SDA, I2C1_SCL);

	init_usb();
	uart_midi_init();

	absolute_time_t now = get_absolute_time();

	// Power-up delay for sh1106
	absolute_time_t next_ui_update = delayed_by_ms(now, 500);

	tac5112_init();
	init_eeprom();

	init_effects();

	// Idle animation setup
	absolute_time_t next_idle_time = delayed_by_ms(now, settings.screensaver);

	multicore_launch_core1(audio_processing);

	for (;;) {
		absolute_time_t now = get_absolute_time();

		if (__atomic_exchange_n(&user_interaction, 0, __ATOMIC_RELAXED))
			next_idle_time = delayed_by_ms(now, settings.screensaver);

#ifdef USB_MODE_HOST
		tuh_task();
#else
		tud_task();
		send_sysex_schema_chunk();
		send_state_dump_chunk();
		usb_audio_task();
#endif
		sh1106_task();
		uart_midi_poll();

		// Claim 25Hz screen updates
		if (now > next_ui_update) {
			next_ui_update = delayed_by_ms(now, 40);
			eeprom_task();

			// Right stomp long-ress: switch to tuner mode
			if (switch_pressed(LONGPRESS(2))) {
				switch_clear(LONGPRESS(2));
				tuner_mode = !tuner_mode;
				send_midi_cc(MIDI_CC_GLOBAL_ENABLE, tuner_mode ? 68 : 69);
			}

			// Are we in tuner mode? Don't do normal
			// screen updates
			if (tuner_mode) {
				tuner_mode_ui();
				forced_update = 1;
				continue;
			}

			if (now > next_idle_time)
				/* screensaver logic */;

			update_ui(forced_update);
			forced_update = 0;

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
