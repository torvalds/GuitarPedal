#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

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
#include "tac5112.h"

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

	//
	// No channel routing, and a merge that is a plain sum if one is
	// ever asked for.  Zero here has to mean "as it always was".
	//
	eff->channels = 0;
	eff->merge = 1.0f;

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

	//
	// Including where it sat across the channels, which is state in
	// exactly the same sense as a pot is and was being kept.  An
	// effect steered to one side and then unrouted came back steered,
	// which is not what "starts from the defaults" says.
	//
	eff->channels = 0;
	eff->merge = 1.0f;

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

//
// The two effects that always run, asked of ROUTABLE_EFFECTS rather than
// spelled out again.
//
// Effect 0 is [CHAIN] - the trim, the gate and the master volume - which
// runs ahead of the chain rather than in it, and the last is the settings
// pseudo-effect, which is not an audio effect at all.  Neither is ever in
// effect_chain[], so "is it routed" is the wrong question to ask about
// them and always gets the wrong answer.
//
// Derived from the routing bitmap instead of testing 0 and EFFECT_COUNT-1
// by hand so that the two cannot drift apart: whatever is not routable is
// what always runs, by construction.
//
// Both happen to be exactly the effects declaring 'MIX: NONE' today, so
// e->no_mix would answer this correctly - by coincidence.  Nothing stops
// a routable effect from having no mix, and then it would not.
//
static bool effect_always_runs(unsigned int id)
{
	return !(ROUTABLE_EFFECTS & ((routing_bitmap_t)1 << id));
}

//
// Is this effect in the chain?
//
// Two callers with their own copy of this loop is how they would come to
// disagree, which is the same reason set_effect_mix() exists at all.
//
static bool effect_is_routed(const struct effect *e)
{
	for (int i = 0; i < routed_effect_count; i++) {
		if (effects[effect_chain[i]] == e)
			return true;
	}
	return false;
}

#include "scene.h"

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

//
// What this firmware is, and what it found itself running on.
//
// The hardware half is probed once at boot.  A fixed build cannot adapt
// to the board it lands on and this does not try to - it answers a
// different question, which has cost an evening more than once: is this
// the board this firmware was built for at all?
//
// The early boards carried a TAC5112 codec with its control registers on
// i2c0, and an SH1106 screen on i2c1.  Neither is supported any more and
// the code for both is gone, but the parts still answer when addressed.
// So anything replying there means the firmware is newer than the board,
// and nothing else in the system is in a position to notice.
//
// The eeprom is in the same position now.  It was the scene store, and
// which part was fitted mattered a great deal - the sizes differ in how
// many address bytes they take, so a mismatched build wrote to the wrong
// place and the pedal ran perfectly and forgot everything on reboot.
// Scenes live in the RP2354's own flash now and nothing reads the part
// at all, so all that is left is the same statement as the other two:
// something is answering at 0x50.
//
// What gets reported is what was *observed*.  Any inference from it -
// which board this is, how old - belongs to whoever is reading rather
// than in the wire format, so that being wrong about it later costs an
// app change and not a protocol one.
//
static struct {
	bool eeprom;		// the old scene store, 0x50
	bool legacy_codec;	// TAC5112, 0x51 - an early board
	bool legacy_screen;	// SH1106, 0x3c - ditto
} hardware;

static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr)
{
	uint8_t byte;

	// One byte, harmless to anything that does answer, and a timeout
	// rather than a hang if the bus is being held down.
	return i2c_read_timeout_us(i2c, addr, &byte, 1, false, 2000) == 1;
}

static void probe_hardware(void)
{
	hardware.eeprom = i2c_probe(MC24Cxx_I2C);
	hardware.legacy_codec = i2c_probe(TAC5112_I2C);
	hardware.legacy_screen = i2c_probe(SH1106_I2C);

	//
	// The one inference drawn from any of this, and it is drawn here
	// rather than on the wire.  A name is allowed to guess: nothing
	// depends on it being right beyond a human reading a port list,
	// and being wrong costs a rebuild.  The identity reply is not
	// allowed to, for the reason above - so it keeps reporting that
	// nothing answered at 0x51, and this says what that means.
	//
	// It has to be said before init_usb(), which is why probing
	// happens as early as it does.
	//
	usb_set_product(hardware.legacy_codec ? "TAC5112 Pedal" : "TAC5242 Pedal");

	//
	// Worst first, and only one of these arrives: report_status() is a
	// plain overwrite and get_status() takes the message away as it
	// reads it, so a chain of ifs would deliver the last thing tested
	// rather than the thing worth saying.
	//
	// An early board is merely old: the TAC5112 wants a little setup,
	// which it gets, and those boards never routed the second
	// channel, so they are mono.
	//
	// A missing eeprom used to be the thing worth saying, because it
	// meant nothing would persist.  It says nothing now - scenes are
	// in the RP2354's flash, which is on the die and cannot be
	// absent - so the boards built without one have stopped being
	// useless and stopped needing a warning.
	//
	if (hardware.legacy_codec || hardware.legacy_screen)
		report_status("Early board: mono only");
}

static void sysex_write_str(const char *str)
{
	sysex_stream_write((const uint8_t *)str, strlen(str));
}

//
// A number, for the JSON below.  Hand-rolled for the same reason
// eeprom.h rolls its own: snprintf() drags newlib's float conversion in
// behind it, and scripts/check-float.py exists to refuse exactly that.
//
static void sysex_write_num(uint32_t val)
{
	char buf[11];
	unsigned int n = sizeof(buf);

	buf[--n] = 0;
	do {
		buf[--n] = '0' + val % 10;
		val /= 10;
	} while (val);

	sysex_write_str(buf + n);
}

//
// Identity, as JSON.
//
// Two kinds of thing come back from the pedal and they want opposite
// encodings.  This one is asked once, when the app connects, so a couple
// of hundred bytes cost nothing and being self-describing means a field
// can be added later without either side agreeing a version first - the
// same bargain the schema already makes.  Anything *polled* is the other
// case and should be packed bytes instead.
//
// Measured on the wire: 208 bytes, one SysEx message.  So "a couple of
// hundred" is exactly right, which is worth writing down because a first
// attempt at measuring it said 1918 - the pedal streams its status CCs
// continuously, so a capture window long enough to be sure of catching a
// reply collects a second of those alongside it, and the inflation
// scales with the window rather than with the reply.  Any figure taken
// off a raw byte count has that in it.
//
bool send_identity_tx = false;
static void sysex_send_identity(void)
{
	if (!send_identity_tx)
		return;
	send_identity_tx = false;

	static const uint8_t sysex_identity_header[] = { 0xF0, 0x7D, 0x0A };
	static const uint8_t sysex_identity_trailer[] = { 0xF7 };

	sysex_tx_start();
	sysex_stream_write(sysex_identity_header, sizeof(sysex_identity_header));

	//
	// The build stamp is what answers "did I actually reflash?", which
	// is the question that gets asked in anger.  It only moves when
	// blink.c is recompiled, which is exactly when the binary changed.
	//
	sysex_write_str("{\"build\":\"" __DATE__ " " __TIME__ "\"");
	//
	// How many scenes there are, and which of them have ever been
	// saved.  The count used to depend on which eeprom was fitted and
	// needed a paragraph explaining how it had been guessed; it is a
	// build constant now, because every board has the same 2MB of
	// flash on the die.
	//
	sysex_write_str(",\"scenes\":");
	sysex_write_num(MAX_SCENES);
	sysex_write_str(",\"populated\":");
	sysex_write_num(populated_scenes());
	sysex_write_str(",\"midi_hw\":");
	sysex_write_str(MIDI_HW ? "true" : "false");
	sysex_write_str(",\"found\":{\"eeprom\":");
	sysex_write_str(hardware.eeprom ? "true" : "false");
	sysex_write_str(",\"legacy_codec\":");
	sysex_write_str(hardware.legacy_codec ? "true" : "false");
	sysex_write_str(",\"legacy_screen\":");
	sysex_write_str(hardware.legacy_screen ? "true" : "false");
	sysex_write_str("}");

	//
	// What is in the save area, read now.  Nothing needs this to
	// run; it is here so that slots planted with picotool can be
	// asked about from a shell, which is the whole test rig for the
	// format - 'marked' counts what carried a marker and 'valid'
	// what also survived its hash, so a deliberately corrupted
	// signature shows up as the difference between the two.
	//
	struct save_scan found;

	save_scan(&found);

	sysex_write_str(",\"save\":{\"slots\":");
	sysex_write_num(SAVE_SLOT_COUNT);
	sysex_write_str(",\"marked\":");
	sysex_write_num(found.marked);
	sysex_write_str(",\"valid\":");
	sysex_write_num(found.valid);
	sysex_write_str(",\"keys\":");
	sysex_write_num(found.keys);
	sysex_write_str(",\"newest\":");
	sysex_write_num(found.newest);
	sysex_write_str("}}");

	sysex_stream_write(sysex_identity_trailer, sizeof(sysex_identity_trailer));
	sysex_tx_finish("Sent identity");
}

//
// Telemetry: what the pedal can see about its own signal.
//
// Packed bytes rather than the JSON the identity reply uses, because this
// is the polled one - asked several times a second while somebody watches
// a meter, where a couple of hundred bytes of punctuation per frame would
// be silly.
//
// *** The layout is append-only. ***
//
// Fields are never reordered, never resized and never repurposed.  A host
// reads the ones it understands and ignores the rest, and treats a frame
// shorter than it expected as "that firmware does not know about the tail"
// rather than as zeroes.  Get that right and every future field is free,
// which is the whole reason this is not self-describing.  The version byte
// is belt and braces - the length is what actually does the work.
//
// Levels go out as -dBFS, one byte per dB.  Full scale is 0 and it counts
// downwards, which suits a 7-bit field: 127dB is more range than the
// converter has, and no sign ever needs sending.
//
static uint8_t level_to_dbfs(float level)
{
	int db;

	// Silence saturates rather than trying to send minus infinity
	if (level <= 0.0f)
		return 127;

	db = -(int)(20.0f * log10f(level));
	if (db < 0)
		db = 0;		// above full scale: still 0dBFS
	if (db > 127)
		db = 127;
	return db;
}

static uint8_t fraction_to_byte(float f)
{
	int v = (int)(f * 127.0f + 0.5f);

	if (v < 0)
		v = 0;
	if (v > 127)
		v = 127;
	return v;
}

bool send_telemetry_tx = false;
static void sysex_send_telemetry(void)
{
	if (!send_telemetry_tx)
		return;
	send_telemetry_tx = false;

	static const uint8_t sysex_telemetry_header[] = { 0xF0, 0x7D, 0x0B };
	static const uint8_t sysex_telemetry_trailer[] = { 0xF7 };

	//
	// The gate multiplier only means anything while the gate is on.
	// Switched off it keeps whatever it last ramped to, which would
	// read as a gate holding the signal down when it is doing nothing
	// of the kind.
	//
	float gate = chain.active ? chain.mult : 1.0f;

	const uint8_t body[] = {
		1,				// layout version
		level_to_dbfs(meter_in),	// input peak, before Trim
		level_to_dbfs(meter_floor),	// the quiet level under it
		level_to_dbfs(meter_out),	// output peak, after Volume
		fraction_to_byte(gate),		// 127 open, 0 fully closed
		fraction_to_byte(meter_load),	// share of the sample period used
	};

	sysex_tx_start();
	sysex_stream_write(sysex_telemetry_header, sizeof(sysex_telemetry_header));
	sysex_stream_write(body, sizeof(body));
	sysex_stream_write(sysex_telemetry_trailer, sizeof(sysex_telemetry_trailer));

	//
	// No sysex_tx_finish().  This is polled, so a frame that does not
	// make it out is replaced by the next one a fifth of a second later,
	// and saying so would be noise about something that fixed itself.
	//
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

	//
	// Always answer, even when there is nothing pending.  A request
	// that gets no reply leaves the host unable to tell "nothing to
	// report" from "the reply went missing", which for a diagnostic is
	// the wrong way round: silence is exactly what you cannot trust
	// when you are already asking why something is quiet.
	//
	// 'status' stays NULL in that case rather than becoming the empty
	// string, because the two are not interchangeable below: putting an
	// empty string back into the mailbox would be a message as far as
	// report_info() is concerned, and would block every later one.
	//
	const char *status = get_status();

	sysex_tx_start();
	sysex_stream_write(sysex_status_header, sizeof(sysex_status_header));
	if (status)
		sysex_stream_write((const uint8_t *)status, strlen(status));
	sysex_stream_write(sysex_status_trailer, sizeof(sysex_status_trailer));

	//
	// Put it back if it didn't get out.  get_status() took it before
	// the transmit was attempted, and usb_midi_write() is best-effort
	// with a 20ms deadline, so a congested link would otherwise destroy
	// the one message that mattered - at what is a plausible moment for
	// whatever is being diagnosed to be happening.
	//
	// report_info() rather than report_status() is the whole of it: it
	// restores the message only if nothing newer has arrived, and
	// something newer is by definition more current.  Not a
	// self-reference either - this puts the original back rather than
	// reporting a new message about the failure, which is why
	// sysex_tx_finish() is still the wrong thing to call here.  That
	// would report a failed status report as a status report, and start
	// a conversation with itself.
	//
	if (sysex_tx_failed && status)
		report_info(status);
}

static void sysex_send_pot_value(int eff, int pot, int value)
{
	// This should never happen. But just in case...
	if (value < 0 || value > 120) value = 0;

	uint8_t sysex_pot_message[] = { 0xF0, 0x7D, 0x03, eff, pot, value, 0xF7 };
	sysex_stream_write(sysex_pot_message, sizeof(sysex_pot_message));
}

//
// Several of one effect's pots in one message.
//
//	F0 7D 03 <eff> [<pot> <val>] ... F7
//
// Seven bytes to carry three is four bytes of framing in every message,
// and the state dump used to send one per pot.  Grouping by effect rather
// than allowing arbitrary (effect, pot, value) triples is what makes each
// extra pot two bytes instead of three, and it costs nothing to arrange
// because the dump already walks effects with their pots inside.
//
// The single-pot case is byte for byte the message that was always sent,
// so this is a superset rather than a change: sysex_send_pot_value()
// above still emits exactly what it did, and one knob being turned needs
// no thought at all.
//
// Big enough for every pot an effect can have at once: the mix, POT_LAST
// real ones, and the three steering pots.
//
struct pot_batch {
	uint8_t buf[4 + 2 * (1 + POT_LAST + 3) + 1];
	unsigned int len;
};

static void pot_batch_start(struct pot_batch *b, int eff)
{
	b->buf[0] = 0xF0;
	b->buf[1] = 0x7D;
	b->buf[2] = 0x03;
	b->buf[3] = eff;
	b->len = 4;
}

static void pot_batch_add(struct pot_batch *b, int pot, int value)
{
	// Same "should never happen" as the single-pot version.
	if (value < 0 || value > 120) value = 0;

	if (b->len + 2 > sizeof(b->buf) - 1)
		return;
	b->buf[b->len++] = pot;
	b->buf[b->len++] = value;
}

static void pot_batch_send(struct pot_batch *b)
{
	// Nothing added is nothing to say, not an empty message.
	if (b->len <= 4)
		return;
	b->buf[b->len++] = 0xF7;
	sysex_stream_write(b->buf, b->len);
}

//
// Say what the physical controls are bound to.
//
// The whole table in one message, because the app wants all of it or
// none of it - a rule only means anything alongside the others that
// share its gesture.  Sent as part of the
// state dump, and again whenever the pedal changes a binding by itself
// - which it does when a gesture is bound to ACT_NEXT_POT, the one
// action whose effect is to move another binding.
//
// The message itself, with no transmit session of its own, so that it
// can go inside somebody else's.
//
// One level of the rule table.
//
// Four of them, and only two can be written: a scene's rules, the
// pedal-wide ones, what those resolve to, and what is compiled in.  The
// last two are answers rather than settings.
//
// RULES_EFFECTIVE is the one worth having on the wire.  It is what a
// gesture actually does, which is not something the app can work out
// from the other two without implementing the shadowing a second time -
// and two implementations of one rule is how the pot defaults and the
// graph Q both went wrong before.
//
// RULES_DEFAULT closes the other gap.  The compiled-in table has never
// been readable at all, so an app showing "this control is unbound"
// could not say what would happen instead.
//
enum rule_level {
	RULES_SCENE,
	RULES_GLOBAL,
	RULES_EFFECTIVE,
	RULES_DEFAULT,
	NR_RULE_LEVELS,
};

static const struct rule *rule_level(unsigned int level, unsigned int *count)
{
	switch (level) {
	case RULES_SCENE:
		*count = nr_scene_rules;
		return scene_rules;
	case RULES_GLOBAL:
		*count = nr_global_rules;
		return global_rules;
	case RULES_EFFECTIVE:
		*count = nr_rules;
		return rules;
	default:
		*count = ARRAY_SIZE(default_rules);
		return default_rules;
	}
}

static void sysex_write_bindings(unsigned int level)
{
	const uint8_t header[] = { 0xF0, 0x7D, 0x0d, level };
	static const uint8_t trailer[] = { 0xF7 };
	uint8_t table[EFFECTIVE_RULES * 6];
	unsigned int count;
	const struct rule *src = rule_level(level, &count);

	for (unsigned int i = 0; i < count; i++) {
		table[i*6 + 0] = src[i].control;
		table[i*6 + 1] = src[i].action;
		table[i*6 + 2] = src[i].effect;
		table[i*6 + 3] = src[i].pot;
		table[i*6 + 4] = src[i].val[0];
		table[i*6 + 5] = src[i].val[1];
	}

	sysex_stream_write(header, sizeof(header));
	sysex_stream_write(table, count * 6);
	sysex_stream_write(trailer, sizeof(trailer));
}

static void sysex_send_bindings(unsigned int level)
{
	sysex_tx_start();
	sysex_write_bindings(level);
	sysex_tx_finish("Sent control bindings");
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

	//
	// Then the effect states - for the effects that have one.
	//
	// An effect that is not in the chain is not running, and
	// routing_end() has reset it to the schema defaults the app
	// already knows, so sending its pots is telling the app what it
	// told us.  It was most of the dump: 1050 of 1099 pot bytes on a
	// board with one effect routed, which is 95% of the traffic
	// describing effects that do nothing.
	//
	// This is only correct while "unrouted means schema defaults" is
	// the model.  If the pedal ever keeps values for effects it is
	// not running, those values become real state and belong here
	// again.
	//
	report_info("Sending effect pot state");
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *e = effects[i];
		const struct pot_descr *desc = e->pots;
		unsigned char *pot_values = e->pot_values[e->seq & 1];
		struct pot_batch batch;

		if (!effect_always_runs(i) && !effect_is_routed(e))
			continue;

		pot_batch_start(&batch, i);

		// We send the mix as "pot 0", and then pots numbered from 1
		pot_batch_add(&batch, POT_MIX, FLOAT_TO_POT(e->mix_pot));
		for (int pot = 0; pot < POT_LAST; pot++) {
			if (!desc[pot].label)
				break;
			pot_batch_add(&batch, pot+1, pot_values[pot]);
		}

		//
		// ...and where it sits across the channels, for the
		// effects that have somewhere to sit.  Sent as pots so
		// that an app which does not know these numbers yet
		// ignores three more of what it was already reading.
		//
		if (!e->no_mix) {
			pot_batch_add(&batch, POT_CH_IN, CH_IN(e->channels));
			pot_batch_add(&batch, POT_CH_OUT, CH_OUT(e->channels));
			pot_batch_add(&batch, POT_MERGE, FLOAT_TO_POT(e->merge));
		}

		pot_batch_send(&batch);
	}

	//
	// What the pedal's own controls are bound to - every level of
	// it.  The dump is what the app builds its whole picture from,
	// and "what this control does" and "why" are different
	// questions that it now has to be able to answer separately.
	//
	report_info("Sending control bindings");
	for (unsigned int level = 0; level < NR_RULE_LEVELS; level++)
		sysex_write_bindings(level);

	//
	// And finally the routing order, which stays last because the app
	// takes it as the end of the dump.
	//
	report_info("Sending routing information");
	static const uint8_t sysex_routing_header[] = { 0xF0, 0x7D, 0x08 };
	static const uint8_t sysex_routing_trailer[] = { 0xF7 };

	sysex_stream_write(sysex_routing_header, sizeof(sysex_routing_header));
	sysex_stream_write(effect_chain, routed_effect_count);
	sysex_stream_write(sysex_routing_trailer, sizeof(sysex_routing_trailer));

	sysex_tx_finish("Sent state dump");
}

//
// Big enough for the largest thing that arrives, which is the rule
// table: six bytes each and a command byte in front.
//
static uint8_t sysex_buf[1 + MAX_RULES * 6];
static int sysex_len = 0;
static bool in_sysex = false;
static bool sysex_over = false;

//
// Change one pot from core 0.
//
// Fill the inactive row and then release the sequence number, so that
// core 1 either sees the old set or the new one and never a half-written
// mixture of the two.  See 'pot_values' in audio/effect.h.
//
static void set_effect_pot(struct effect *e, unsigned int pot_idx, unsigned char val)
{
	unsigned int seq = e->seq;
	unsigned char *cur_pot = e->pot_values[seq & 1];
	unsigned char *new_pot = e->pot_values[!(seq & 1)];

	memcpy(new_pot, cur_pot, 10);
	new_pot[pot_idx] = val;
	smp_store_release(&e->seq, seq + 1);
}

//
// Change an effect's mix from core 0.
//
// Not just a parameter write: an effect that is in the chain has to be
// stepped at all for its mix to mean anything, so setting the mix also
// re-asserts whether it runs.  Being in the chain is the whole of that
// now - both ends of effects[] used to be listed here as honorary chain
// members, the signal chain because it always runs and settings because
// forcing 'target' was how it kept its init() scheduled, and neither has
// a wet or a dry for this to be about.
//
// It lives here rather than inside the SysEx handler because a binding
// can set the mix too, and two callers with their own idea of what that
// entails is how the two would come to disagree.
//
static void set_effect_mix(struct effect *e, unsigned char val)
{
	set_mix_pot(e, POT_TO_FLOAT(val));

	if (!e->no_mix)
		e->target = effect_is_routed(e) ? EFF_ENABLE_STEPS : 0;
}

//
// Where an effect sits across the two channels, from core 0.
//
// Deliberately not the double-buffered dance set_effect_pot() does.
// That exists because ten values have to become visible to core 1 as a
// set - half of an old pot array and half of a new one is a filter
// nobody asked for - and these are single scalars that each mean
// something on their own.  The worst a change between one sample and the
// next can do is a click, which is what moving a signal from one side to
// the other sounds like however carefully it is done.
//
// 'merge' is a float and a plain store of one is atomic here, so core 1
// sees the old value or the new one and never a mixture.
//
static void set_effect_steering(struct effect *e, unsigned int pot,
				unsigned char val)
{
	unsigned char ch = e->channels;

	switch (pot) {
	case POT_CH_IN:
		e->channels = (ch & ~3) | (val & 3);
		break;
	case POT_CH_OUT:
		e->channels = (ch & ~(3 << 2)) | ((val & 3) << 2);
		break;
	case POT_MERGE:
		e->merge = POT_TO_FLOAT(val);
		break;
	}
}

static void handle_sysex_payload(uint8_t *sysex_buf, size_t sysex_len)
{
	uint8_t cmd = sysex_buf[0];
	if (cmd == 0x01) { // Schema Request
		send_schema_tx = true;
	} else if (cmd == 0x03 && sysex_len >= 4) { // Set Parameter
		//
		// One effect, and then as many (pot, value) pairs as the
		// message carries - the same shape the state dump sends.
		//
		// An odd number of bytes after the effect id is a message
		// somebody built wrong, and the last half-pair is dropped
		// rather than guessed at.  A message too long to fit was
		// already refused whole by the accumulator, so a short
		// list here is a short list somebody meant to send.
		//
		uint8_t eff_id = sysex_buf[1];
		struct effect *e = NULL;
		if (eff_id < ARRAY_SIZE(effects)) {
			e = effects[eff_id];
		}
		for (size_t i = 2; e && i + 1 < sysex_len; i += 2) {
			uint8_t pot_idx = sysex_buf[i];
			uint8_t val = sysex_buf[i + 1];

			if (pot_idx == POT_MIX)
				set_effect_mix(e, val);
			else if (pot_idx <= POT_LAST)
				set_effect_pot(e, pot_idx - 1, val);
			//
			// Steering is meaningless for an effect that
			// is not stepped through do_effect_step() at
			// all, which is what no_mix marks.
			//
			else if (pot_idx <= POT_MAX && !e->no_mix)
				set_effect_steering(e, pot_idx, val);
		}
	} else if (cmd == 0x04 && sysex_len >= 2) { // Save Scene
		uint8_t scene_id = sysex_buf[1];

		//
		// The settings go too, even though they are no longer
		// part of the scene.  They used to be saved by being in
		// its last slot, and "Save" meaning "keep what I have
		// set up" is what anybody pressing it expects - the
		// change is where they are kept, not when.
		//
		save_scene(scene_id);
		save_globals();

	} else if (cmd == 0x09) { // Diagnostic Request

		send_status_tx = true;

	} else if (cmd == 0x0a) { // Identity Request

		send_identity_tx = true;

	} else if (cmd == 0x0b) { // Telemetry Request

		send_telemetry_tx = true;

	} else if (cmd == 0x05) { // State Dump Request

		state_dump_tx = true;

	} else if (cmd == 0x0c && sysex_len >= 2) { // Write one rule level
		unsigned int level = sysex_buf[1];

		//
		// Echo it back, always, and not as an acknowledgement:
		// rules that do not check out are dropped, so what came
		// back is the answer to "what did you keep".  That is
		// also the case where the two ends disagree, which is
		// exactly when the app needs telling.
		//
		// A write to a level that is computed rather than kept
		// is refused rather than ignored - it still answers, with
		// the level unchanged, so the app sees that nothing
		// happened instead of assuming it worked.
		//
		if (level == RULES_SCENE || level == RULES_GLOBAL) {
			struct rule *dst = level == RULES_SCENE ?
				scene_rules : global_rules;
			unsigned int *n = level == RULES_SCENE ?
				&nr_scene_rules : &nr_global_rules;

			set_rules(dst, n, sysex_buf + 2, (sysex_len - 2) / 6);
		}
		if (level < NR_RULE_LEVELS)
			sysex_send_bindings(level);

	} else if (cmd == 0x0d && sysex_len >= 2) { // Read one rule level
		//
		// Separate from the write, because "set the table to
		// nothing" and "tell me the table" would otherwise be
		// the same message - and clearing a level by asking to
		// look at it is a memorable way to lose work.
		//
		// Same command number as the reply, which is how 0x0a
		// and 0x0b already work.
		//
		if (sysex_buf[1] < NR_RULE_LEVELS)
			sysex_send_bindings(sysex_buf[1]);

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
				sysex_over = false;
			} else if (b == 0xF7 && in_sysex) {
				in_sysex = false;
				//
				// A message that did not fit is dropped whole
				// rather than acted on short.
				//
				// The bytes past the end used to be discarded
				// while the rest was handled anyway, which is
				// only harmless while every message is a fixed
				// size - a truncated one then fails its length
				// check and goes nowhere.  It stops being
				// harmless the moment a message carries a list,
				// because a short list is a valid shorter list:
				// half the pots in a batch would be set and the
				// other half silently ignored.
				//
				if (sysex_over)
					report_info("MIDI message too long, dropped");
				else
					handle_sysex_payload(sysex_buf, sysex_len);
			} else if (in_sysex) {
				if (sysex_len == 0 && b == 0x7D) {
					// Consume header 7D
				} else if (sysex_len < sizeof(sysex_buf)) {
					sysex_buf[sysex_len++] = b;
				} else {
					sysex_over = true;
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
		} else if (data1 == 7) { // Main volume
			//
			// The far end of the signal chain.  CC data is
			// 0-127 and a pot is 0-120, so scale rather than
			// clamp - a controller at full should mean unity,
			// not seven counts short of it.
			//
			set_effect_pot(effects[0], CHAIN_VOLUME,
				       data2 * 120 / 127);
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
// The one rotary encoder.  Turning it changes the selected pot's value,
// and that is all a turn has ever meant to anything but the old EQ.
//
// Accumulated by the interrupt, drained by update_ui().  There used to
// be a second encoder for picking the effect; it is gone, and picking
// the effect is done over MIDI.
//
static volatile int rotary_value;

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

		rotary_value += val;
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
	init_meters();
	for (;;)
		make_one_noise();
}

unsigned get_audio_samples(int32_t *buffer, unsigned nr)
{
	return get_output_samples((s32 *)buffer, nr);
}

static void init_effects(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		reset_effect(effect);
	}

	//
	// The settings first, because they are the pedal's and not the
	// scene's - which of them is the MIDI channel should not depend
	// on which scene happens to load next.
	//
	load_globals();

	//
	// No fallback for a store with nothing in it, because there is
	// nothing to fall back to.  A slot that fails its hash, or was
	// never written, simply does not load and every effect keeps the
	// defaults reset_effect() just gave it - which leaves a new pedal
	// with everything at its default and nothing routed.  That is the
	// right answer, and guessing a chain would be worse.
	//
	load_scene(0);

	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		effect->init(effect->pot_values[0]);
	}
}

#include "tuner.h"

//
// How long boot is allowed to take before it is called a hang.
//
// Everything between here and the main loop is i2c probes with 2ms
// timeouts, some table building and a flash read, so the real figure is
// milliseconds and this is all margin.  It has to stay well clear of
// the truth in the other direction too: a boot that legitimately took
// longer than this would reach BOOTSEL instead of playing, which is a
// worse failure than the one being guarded against.
//
#define BOOT_WATCHDOG_MS 3000

int main()
{
	//
	// A pedal that hangs before its main loop is a brick.
	//
	// It has no USB, so it is not a device and not in BOOTSEL and the
	// host logs nothing at all - from the other end nothing was ever
	// plugged in.  It has no audio either, so the only way back is the
	// BOOTSEL button, and on a board in an enclosure that is a screw-
	// driver.  This has been seen occasionally for a long time without
	// ever being pinned down, which is partly because every occurrence
	// destroys the evidence and costs a disassembly.
	//
	// So the hang is made survivable rather than diagnosed: arm a
	// watchdog before anything that could hang, and if the previous
	// boot never got far enough to disarm it, ask the bootrom for
	// BOOTSEL instead of trying again.  A hung pedal then comes back
	// as something picotool can talk to, and the next attempt costs a
	// reflash rather than a screwdriver.
	//
	// watchdog_enable_caused_reboot() is specifically a *timeout*, not
	// any reset - a deliberate watchdog_reboot() sets a different
	// magic - so this cannot be tripped by anything asking for a
	// restart on purpose.
	//
	if (watchdog_enable_caused_reboot())
		reset_usb_boot(0, 0);

	watchdog_enable(BOOT_WATCHDOG_MS, false);

	enable_ftz();

	init_i2s();
	init_ws2812();
	init_sw_pins();
	init_pwm_pins();
	init_rotary_encoder();
	init_i2c_bus(i2c0, 400, I2C0_SDA, I2C0_SCL);
	init_i2c_bus(i2c1, 400, I2C1_SDA, I2C1_SCL);

	//
	// Before init_usb(), because it decides what the pedal enumerates
	// as.  USB is a hotplug bus and the host may already be attached
	// and waiting, so the name wants to exist before the device does.
	// The i2c buses above are all this needs.
	//
	probe_hardware();

	init_usb();
	uart_midi_init();

	absolute_time_t now = get_absolute_time();

	absolute_time_t next_ui_update = delayed_by_ms(now, 50);

	//
	// Early boards need their codec set up; the current ones strap it
	// in hardware.  Unconditional because the first thing it does is
	// ask whether there is a TAC5112 there to talk to, which is the
	// same question as whether this is one of those boards.
	//
	tac5112_init();

	init_effects();

	multicore_launch_core1(audio_processing);

	//
	// Booted.  Everything from here is the main loop, which has its own
	// ways of going wrong and is not what this was guarding.
	//
	watchdog_disable();

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

		sysex_send_identity();
		sysex_send_telemetry();
		sysex_send_schema();
		sysex_send_state_dump();
		sysex_send_status();
		usb_audio_task();

		// Claim 25Hz screen updates
		if (now > next_ui_update) {
			next_ui_update = delayed_by_ms(now, 40);

			//
			// Whatever the switches are bound to.
			//
			// Ahead of the tuner check on purpose: update_ui()
			// does not run in tuner mode, and a gesture bound to
			// ACT_TUNER has to be able to turn it off again.  A
			// side effect is that a switch now acts while the
			// tuner is up rather than being queued until it is
			// dismissed, which is the more predictable of the
			// two behaviours anyway.
			//
			handle_switch_bindings();

			// Are we in tuner mode?
			if (tuner_mode) {
				tuner_mode_ui();
				continue;
			}

			update_ui();
		}
	}
}
