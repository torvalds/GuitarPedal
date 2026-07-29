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

#include "status.h"
#include "ws2812.pio.h"
#include "debounce.pio.h"
#include "rotary.pio.h"
#include "i2s.pio.h"

#define PIO0_I2S_TX_SM 0
#define PIO0_I2S_RX_SM 1
#define PIO0_WS2812_SM 2

// PIO1 runs one debounce state machine per switch, and the state
// machine index is the switch id - see switch.h.  PIO2 has the one
// rotary encoder.
#define ROTARY_SM 0

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
#include "switch.h"

static int tuner_mode = 0;
static volatile int user_interaction = 0;
static volatile int next_state_seq = 1;

#include "audio/effect.h"

uint8_t effect_chain[MAX_ROUTED_EFFECTS];
uint8_t routed_effect_count = 0;

static void reset_effect(struct effect *eff)
{
	eff->active_pot = 0;
	eff->last = -1;
	eff->seq = 0;
	eff->mix = eff->target = 0;
	eff->dry = 1.0f;
	eff->wet = 0.0f;
	set_mix_pot(eff, eff->def_mix);
	for (int i = 0; i < 10; i++) {
		unsigned char def_val = eff->pots[i].def_val;
		eff->pot_values[0][i] = def_val;
		eff->pot_values[1][i] = def_val;
	}
}

//
// Unrouting an effect throws its values away.  An effect that isn't in
// the chain isn't supposed to have any state at all, so routing it again
// starts from the defaults in the schema rather than from wherever it
// happened to be left.
//
// The pot values go through the usual double-buffer dance - fill the
// inactive set, publish it by bumping 'seq' - but the mix is just slammed
// to zero, because an unrouted effect isn't stepped at all any more and
// so has nothing left to ramp it down.  That clicks.  That's fine: you
// route effects while setting the pedal up, not while playing.
//
static void unroute_effect(struct effect *eff)
{
	unsigned int seq = eff->seq;
	unsigned char *new_pot = eff->pot_values[!(seq & 1)];

	for (int i = 0; i < 10; i++)
		new_pot[i] = eff->pots[i].def_val;

	set_mix_pot(eff, eff->def_mix);
	eff->mix = eff->target = 0;
	eff->dry = 1.0f;
	eff->wet = 0.0f;
	smp_store_release(&eff->seq, seq + 1);
}

//
// Building the routing chain.
//
// Effects get added one at a time out of a bitmask of what is still
// available, which makes it structurally impossible to route the same
// effect twice or to route something that isn't a routable effect at
// all.  Both used to be possible, and both used to walk off the end of
// effect_chain[].
//
// Bit N set means effect N can still be added.  The gate and the
// settings pseudo-effect are never in the mask: the gate always runs
// first, outside the chain, and settings isn't an audio effect.
//
typedef uint32_t routing_bitmap_t;

_Static_assert(EFFECT_COUNT <= 32, "routing_bitmap_t is too narrow for this many effects");

// Bits 1 .. EFFECT_COUNT-2, ie everything that can go in the chain.
#define ROUTABLE_EFFECTS ((routing_bitmap_t)((1u << (EFFECT_COUNT - 1)) - 2))

static routing_bitmap_t routing_start(void)
{
	routed_effect_count = 0;
	return ROUTABLE_EFFECTS;
}

static bool routing_add(routing_bitmap_t *routable, uint8_t eff_id)
{
	if (eff_id >= EFFECT_COUNT)
		return false;
	if (!(*routable & (1u << eff_id)))
		return false;
	if (routed_effect_count >= MAX_ROUTED_EFFECTS)
		return false;

	*routable &= ~(1u << eff_id);
	effect_chain[routed_effect_count++] = eff_id;
	effects[eff_id]->target = EFF_ENABLE_STEPS;
	return true;
}

//
// Whatever is left in the bitmap didn't get routed, so throw it away.
//
static void routing_end(routing_bitmap_t routable)
{
	while (routable) {
		unroute_effect(effects[__builtin_ctz(routable)]);
		routable &= routable - 1;
	}
}

#include "eeprom.h"

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
#ifdef WS2812_GPIO
	uint offset = pio_add_program(pio0, &ws2812_program);
	ws2812_program_init(pio0, PIO0_WS2812_SM, offset, WS2812_GPIO);
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
	return tud_ready();
}

// We use PIO1 for the switches.
//
// They share the same program, just a separate state machine
// for each pin - state machine N is switch id N, see switch.h.
static void switch_irq(void)
{
	PIO pio = pio1;

	for (int sw = 0; sw < NR_SWITCHES; sw++) {
		if (pio_sm_is_rx_fifo_empty(pio, sw))
			continue;

		int bit = pio_sm_get(pio, sw) ? LONGPRESS(sw) : sw;
		switch_val |= 1u << bit;
	}

	user_interaction = 1;
}

int current_midi_effect_idx = 0;

#include "midi_schema.h"

extern bool usb_midi_write(const uint8_t packet[4]);

static uint8_t sysex_pack_buf[3];
static int sysex_pack_len = 0;
static bool sysex_pack_active = false;

//
// A SysEx message is many packets, and a transmit that gives up part-way
// through one would leave the rest of it stranded.  So the first failure
// abandons the whole message: every later write in it turns into a
// no-op, and no 0xF7 goes out.
//
// Abandoning is better than truncating.  Web MIDI only delivers SysEx
// messages that were terminated, so a message that never ends is one the
// app simply never sees, and it can ask again.  A truncated message with
// an 0xF7 stuck on the end would arrive looking complete and be parsed
// as garbage - half a JSON schema, say.
//
static bool sysex_tx_failed = false;

static void sysex_tx_start(void)
{
	sysex_tx_failed = false;
	sysex_pack_active = false;
	sysex_pack_len = 0;
}

static void sysex_tx_finish(const char *sent)
{
	report_info(sysex_tx_failed ? "MIDI transmit stalled, message dropped" : sent);
}

static void sysex_stream_write(const uint8_t *buffer, size_t len)
{
	if (sysex_tx_failed)
		return;

	for (size_t i = 0; i < len; i++) {
		uint8_t b = buffer[i];
		if (b == 0xF0) {
			sysex_pack_active = true;
			sysex_pack_len = 0;
		}
		if (sysex_pack_active) {
			sysex_pack_buf[sysex_pack_len++] = b;
			if (b == 0xF7) {
				uint8_t packet[4] = { (uint8_t)(0x04 + sysex_pack_len), 0, 0, 0 };
				for (int j = 0; j < sysex_pack_len; j++) packet[1+j] = sysex_pack_buf[j];
				sysex_pack_active = false;
				sysex_pack_len = 0;
				if (!usb_midi_write(packet)) {
					sysex_tx_failed = true;
					return;
				}
			} else if (sysex_pack_len == 3) {
				uint8_t packet[4] = { 0x04, sysex_pack_buf[0], sysex_pack_buf[1], sysex_pack_buf[2] };
				sysex_pack_len = 0;
				if (!usb_midi_write(packet)) {
					sysex_tx_failed = true;
					sysex_pack_active = false;
					return;
				}
			}
		}
	}
}

bool send_schema_tx = false;
static void sysex_send_schema(void)
{
	if (!send_schema_tx)
		return;
	send_schema_tx = false;

	static const uint8_t sysex_schema_header[] = { 0xF0, 0x7D, 0x02 };
	static const uint8_t sysex_schema_trailer[] = { 0xF7 };

	sysex_tx_start();
	sysex_stream_write(sysex_schema_header, sizeof(sysex_schema_header));
	sysex_stream_write((const uint8_t *)midi_schema_json, strlen(midi_schema_json));
	sysex_stream_write(sysex_schema_trailer, sizeof(sysex_schema_trailer));
	sysex_tx_finish("Sent schema information");
}

bool send_status_tx = false;
static void sysex_send_status(void)
{
	if (!send_status_tx)
		return;
	send_status_tx = false;

	static const uint8_t sysex_status_header[] = { 0xF0, 0x7D, 0x09 };
	static const uint8_t sysex_status_trailer[] = { 0xF7 };

	const char *status = get_status();
	if (!status)
		return;

	sysex_tx_start();
	sysex_stream_write(sysex_status_header, sizeof(sysex_status_header));
	sysex_stream_write((const uint8_t *)status, strlen(status));
	sysex_stream_write(sysex_status_trailer, sizeof(sysex_status_trailer));

	// Not sysex_tx_finish(): reporting a failed status report as a
	// status report is how you get an endless conversation with
	// yourself.  The next one will go out or it won't.
}

static void sysex_send_pot_value(int eff, int pot, int value)
{
	// This should never happen. But just in case...
	if (value < 0 || value > 120) value = 0;

	uint8_t sysex_pot_message[] = { 0xF0, 0x7D, 0x03, eff, pot, value, 0xF7 };
	sysex_stream_write(sysex_pot_message, sizeof(sysex_pot_message));
}

bool state_dump_tx = false;
static void sysex_send_state_dump(void)
{
	if (!state_dump_tx)
		return;
	state_dump_tx = false;

	// Send the global enable state.  A plain CC rather than SysEx,
	// and if it will not go then nobody is reading and there is no
	// point starting on the rest.
	report_info("Sending global-enable state");
	uint8_t cc_packet[4] = { 0x0B, 0xB0, MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127 };
	if (!usb_midi_write(cc_packet))
		return;

	//
	// One give-up flag for the whole dump rather than one per
	// message.  It is a couple of hundred messages, so retrying each
	// in turn against a host that has stopped reading would block
	// core 0 for the timeout times the message count - seconds.
	// Once it is set every write below quietly does nothing.
	//
	sysex_tx_start();

	// Then send the effect states
	report_info("Sending effect pot state");
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *e = effects[i];
		const struct pot_descr *desc = e->pots;
		unsigned char *pot_values = e->pot_values[e->seq & 1];

		// We send the mix as "pot 0", and then pots numbered from 1
		sysex_send_pot_value(i, 0, FLOAT_TO_POT(e->mix_pot));
		for (int pot = 0; pot < 10; pot++) {
			if (!desc[pot].label)
				break;
			sysex_send_pot_value(i, pot+1, pot_values[pot]);
		}
	}

	// And finally, send the routing order
	report_info("Sending routing information");
	static const uint8_t sysex_routing_header[] = { 0xF0, 0x7D, 0x08 };
	static const uint8_t sysex_routing_trailer[] = { 0xF7 };

	sysex_stream_write(sysex_routing_header, sizeof(sysex_routing_header));
	sysex_stream_write(effect_chain, routed_effect_count);
	sysex_stream_write(sysex_routing_trailer, sizeof(sysex_routing_trailer));

	sysex_tx_finish("Sent state dump");
}

static uint8_t sysex_buf[32];
static int sysex_len = 0;
static bool in_sysex = false;

static void handle_sysex_payload(uint8_t *sysex_buf, size_t sysex_len)
{
	uint8_t cmd = sysex_buf[0];
	if (cmd == 0x01) { // Schema Request
		send_schema_tx = true;
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
				set_mix_pot(e, POT_TO_FLOAT(val));

				bool routed = (e == effects[0] || e == effects[EFFECT_COUNT - 1]);
				for (int i = 0; !routed && i < routed_effect_count; i++) {
					if (effects[effect_chain[i]] == e) routed = true;
				}
				e->target = routed ? EFF_ENABLE_STEPS : 0;
			} else if (pot_idx <= 10) {
				unsigned int seq = e->seq;
				unsigned char *cur_pot = e->pot_values[seq & 1];
				unsigned char *new_pot = e->pot_values[!(seq & 1)];
				memcpy(new_pot, cur_pot, 10);
				new_pot[pot_idx - 1] = val;
				smp_store_release(&e->seq, seq + 1);
			}
		}
	} else if (cmd == 0x04 && sysex_len >= 2) { // Save Scene
		uint8_t scene_id = sysex_buf[1];
		save_scene(scene_id);

	} else if (cmd == 0x09) { // Diagnostic Request

		send_status_tx = true;

	} else if (cmd == 0x05) { // State Dump Request

		state_dump_tx = true;

	} else if (cmd == 0x08) { // Set Routing Order
		routing_bitmap_t routable = routing_start();

		for (int i = 1; i < sysex_len; i++)
			routing_add(&routable, sysex_buf[i]);

		routing_end(routable);
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
		} else if (data1 == 7) { // Volume
			// Not yet wired globally
		} else if (data1 == MIDI_CC_ACTIVE_POT) {
			if (current_midi_effect_idx < ARRAY_SIZE(effects)) {
				effects[current_midi_effect_idx]->active_pot = data2;
			}
		}
	} else if ((status & 0xF0) == 0xC0) {
		handled = true;
		// Program Change -> Load Scene
		if (data1 < MAX_SCENES) {
			load_scene(data1);
		}
	}

	return handled;
}

static void init_sw_pins(void)
{
	PIO pio = pio1;
	uint offset = pio_add_program(pio, &debounce_program);

	//
	// Same PIO program for every switch, one state machine each,
	// walked in switch id order so that state machine N really is
	// switch N.  switch_irq() relies on that and has no other way
	// to know which pin a fifo entry came from.
	//
	for (int sw = 0; sw < NR_SWITCHES; sw++) {
		init_sw_pin(pio, switch_gpio[sw]);
		debounce_program_init(pio, sw, offset, switch_gpio[sw]);
	}

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
	init_one_pwm_pin(LED_GPIO);
	pwm_set_gpio_level(LED_GPIO, 0);
}

static void init_i2c_bus(i2c_inst_t *i2c, int kbps, int sda, int scl)
{
	i2c_init(i2c, kbps * 1000);
	gpio_set_function(sda, GPIO_FUNC_I2C);
	gpio_set_function(scl, GPIO_FUNC_I2C);
	gpio_pull_up(sda);
	gpio_pull_up(scl);
}

//
// The one rotary encoder.  Which of these a click lands in depends on
// whether the shaft is held down at the time:
//
//	turn		change the selected pot's value
//	press and turn	select a different pot
//
// Accumulated by the interrupt, drained by update_ui().  There used to
// be a second encoder for picking the effect; it is gone, and picking
// the effect is done over MIDI.
//
static volatile int rotary_value;
static volatile int rotary_select;

static void rotary_irq(void)
{
	// Initial impossible previous value
	static int prev_value = 4;
	static const int lookup[32] = {
		// CW: 00 -> 10 -> 11 -> 01 -> 00
		[2] = 1, [11] = 1, [13] = 1, [4] = 1,
		// CCW: 00 -> 01 -> 11 -> 10 -> 00
		[1] = -1, [7] = -1, [14] = -1, [8] = -1
	};

	while (!pio_sm_is_rx_fifo_empty(pio2, ROTARY_SM)) {
		int curr = pio_sm_get(pio2, ROTARY_SM) & 3;
		int prev = prev_value;

		int val = lookup[(prev << 2) | curr];
		prev_value = curr;

		if (!val)
			continue;

		// Held down while turning means "pick a pot" rather
		// than "change this one".  Pull-up, so low is pressed.
		if (gpio_get(ROTARY_SW_GPIO))
			rotary_value += val;
		else
			rotary_select += val;
	}
	user_interaction = 1;
}

// We'll use a separate PIO program for the rotary
// encoder pins eventually
static void init_rotary_encoder(void)
{
	PIO pio = pio2;
	uint offset = pio_add_program(pio, &rotary_program);

	// The program reads both pins of the quadrature pair starting
	// at the one it is given, so A and B have to stay adjacent.
	_Static_assert(ROTARY_B_GPIO == ROTARY_A_GPIO + 1,
		       "the quadrature pair has to be adjacent");

	init_sw_pin(pio, ROTARY_A_GPIO);
	init_sw_pin(pio, ROTARY_B_GPIO);
	rotary_program_init(pio, ROTARY_SM, offset, ROTARY_A_GPIO);

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

static void __audio_func(audio_processing)(void)
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
	}

	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		effect->init(effect->pot_values[0]);
	}
}

#include "tuner.h"

int main()
{
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

	absolute_time_t next_ui_update = delayed_by_ms(now, 50);

	init_eeprom();

	init_effects();

	multicore_launch_core1(audio_processing);

	for (;;) {
		absolute_time_t now = get_absolute_time();

		//
		// Everything the outside world asks for is taken in
		// here, and acted on here, so a sender further down can
		// never have the state it is reporting changed under it.
		//
		tud_task();
		usb_midi_poll();
		uart_midi_poll();

		sysex_send_schema();
		sysex_send_state_dump();
		sysex_send_status();
		usb_audio_task();

		// Claim 25Hz screen updates
		if (now > next_ui_update) {
			next_ui_update = delayed_by_ms(now, 40);
			eeprom_task();

			// Stomp held down: switch to tuner mode
			if (switch_pressed(LONGPRESS(STOMP_SWITCH))) {
				switch_clear(LONGPRESS(STOMP_SWITCH));
				tuner_mode = !tuner_mode;
				send_midi_cc(MIDI_CC_GLOBAL_ENABLE, tuner_mode ? 68 : 69);
			}

			// Are we in tuner mode?
			if (tuner_mode) {
				tuner_mode_ui();
				continue;
			}

			update_ui();

			unsigned int current_dropped = __atomic_exchange_n(&samples_dropped, 0, __ATOMIC_RELAXED);
			if (current_dropped) {
				int midi_dropped = current_dropped;
				if (midi_dropped > 127) midi_dropped = 127;
				send_midi_cc(MIDI_CC_CPU_LATENCY, midi_dropped);
			}
		}
	}
}
