//
// This is the "ui" for now - really just for very random testing
//
#include "eeprom.h"

//
// Step to the next pot that exists, wrapping.
//
// Only ever forwards.  Going backwards was what "hold the shaft down
// and turn" did, and that gesture is gone - see read_pots().
//
static void next_pot(struct effect *effect)
{
	int new_active = effect->active_pot;
	do {
		if (++new_active > 9)
			new_active = 0;
		if (new_active == effect->active_pot)
			return;
	} while (!effect->pots[new_active].label);
	effect->active_pot = new_active;
}

struct pot_range { int min, max; };

static const struct pot_range get_pot_range(const struct pot_descr *pot)
{
	int min = 0, max = 120;

	if (pot->enum_names) {
		min = 0;
		for (max = 0; pot->enum_names[max+1]; max++)
			/* nothing */;
	}
	return (struct pot_range) { min, max };
}

// Note that the "__atomic" part isn't actually about SMP, just the
// interrupts
static bool read_pots(struct effect *effect, unsigned char *pots)
{
	const struct pot_descr *pot = effect->pots + effect->active_pot;
	const struct pot_range range = get_pot_range(pot);

	// For small ranges, don't make the rotary so twitchy
	int ignore_low_bits = 0;
	if (range.max - range.min < 25)
		ignore_low_bits = 2;

	int mask = (1 << ignore_low_bits)-1;
	int val = __atomic_fetch_and(&rotary_value, mask, __ATOMIC_RELAXED);
	val >>= ignore_low_bits;

	if (!val) {
		//
		// No turn.  Shaft pressed?
		//
		// The long press is cleared along with the short one
		// because nothing is bound to it, not because it is
		// noise.  It used to be noise: holding the shaft down
		// in order to turn it manufactured a long press every
		// time, which is why the two gestures could not coexist
		// and why hold-and-turn had to go before this bit could
		// ever mean anything.
		//
		unsigned int both = (1u << ROTARY_SWITCH) | (1u << LONGPRESS(ROTARY_SWITCH));
		unsigned int sw = __atomic_fetch_and(&switch_val, ~both, __ATOMIC_RELAXED);
		if (!(sw & (1u << ROTARY_SWITCH)))
			return false;
		next_pot(effect);
		return true;
	}

	val += pots[effect->active_pot];
	if (val < range.min)
		val = range.min;
	else if (val > range.max)
		val = range.max;

	pots[effect->active_pot] = val;

	return true;
}


// Human perception isn't linear, but neither
// is LED intensity, particularly since we're
// typically driving the LED at the lower range
// of the current range
//
// Random map from 0..1 to 0..4096 that works
// for the LED I have happened to pick
static int led_pwm_mapping(float pwm)
{
	return lrintf(pwm * sqrtf(pwm) * PWM_WRAP);
}

//
// Drive the one LED from the same status the host is given.
//
// It takes the bits rather than a single 'intense' flag it could have
// been handed instead, and that is the whole point of the shape.  This
// LED is a WS2812B on the next board, with colours to spend on telling a
// closed gate from a dropped sample, and this is the function that will
// spend them.  Giving it everything now means that change is local to
// here rather than a new argument list and a new caller.
//
// What it can say today is bright or not, so:
//
//  - faults always count.  Clipping and a missed deadline are wrong
//    whatever else is going on.
//
//  - effect activity counts only while the pedal is in circuit.  The
//    chain still runs when bypassed - make_one_noise() keeps stepping it
//    and crossfades the result away - so the compressor goes on
//    compressing into an output nobody hears, and that is not news.
//
//  - the attention preview counts because it *is* the thing being set;
//    see status.h.
//
static void set_led(int pin, bool on, uint8_t global, unsigned int chain)
{
	bool fault = global & (STATUS_DROPPED_MASK | STATUS_CLIPPED);
	bool activity = (global & STATUS_FRONT_ATTN) || chain;
	bool intense = fault || attention_preview || (on && activity);
	int level = 0;

	if (on || intense) {
		float pwm = intense ? settings.led_intense : settings.led_pwm;
		level = led_pwm_mapping(pwm);
	}

	pwm_set_gpio_level(pin, level);
}

_Static_assert(MAX_ROUTED_EFFECTS <= 2 * STATUS_CHAIN_BITS,
	"a chain this long needs a third status CC");

//
// How often to say it again when nothing has changed.
//
// On change alone is not enough, for two reasons that have nothing to do
// with each other.  usb_midi_write() is best-effort and gives up after
// 20ms, so a report can simply be dropped - and a host that missed the
// one message would go on believing the old answer forever, because from
// here nothing has changed since.  And an app that connects while
// something is already wrong never gets told at all, for the same
// reason: it wasn't listening when it changed.
//
// Repeating every eighth tick is about nine messages a second, which is
// nothing next to a SysEx state dump, and makes both cases correct
// themselves within a third of a second.
//
#define STATUS_REPEAT_TICKS 8

//
// Work out what the pedal is doing, and say so - to the LED and to the
// host, which are two renderings of the one answer.  See midi.h for what
// goes in which bit.
//
// Both go out from here rather than the LED being driven separately,
// because the two used to disagree: the LED knew about clipping and lost
// samples while the effects' own activity went nowhere at all, having
// been aimed at a second LED that the current board does not have.
//
// Clearing 'intense' for every effect rather than just the one being
// edited is what makes the chain bits mean anything.  The audio core
// sets them at 48kHz and this is the only thing that ever puts them
// back, so an effect that has stopped asking for attention would
// otherwise stay lit for good - which is exactly what boost, compressor
// and echo had been doing, unnoticed, because nothing read them.
//
static void show_status(void)
{
	unsigned int dropped = __atomic_exchange_n(&samples_dropped, 0,
						   __ATOMIC_RELAXED);
	uint8_t global = dropped > STATUS_DROPPED_MASK
		       ? STATUS_DROPPED_MASK : dropped;
	unsigned int attn = 0;

	if (output_clipped)
		global |= STATUS_CLIPPED;
	if (effects[0]->intense)
		global |= STATUS_FRONT_ATTN;

	for (int i = 0; i < routed_effect_count; i++) {
		if (effects[effect_chain[i]]->intense)
			attn |= 1u << i;
	}

	set_led(LED_GPIO, !disable_all, global, attn);

	// A CC value is seven bits, so the chain needs two of them
	uint8_t chain[2] = { attn & ((1u << STATUS_CHAIN_BITS) - 1),
			     attn >> STATUS_CHAIN_BITS };

	static uint8_t last_global = 0;
	static uint8_t last_chain[2] = { 0, 0 };
	static unsigned int tick;

	bool again = (tick++ % STATUS_REPEAT_TICKS) == 0;

	if (again || global != last_global) {
		send_midi_cc(MIDI_CC_STATUS_GLOBAL, global);
		last_global = global;
	}
	if (again || chain[0] != last_chain[0]) {
		send_midi_cc(MIDI_CC_STATUS_CHAIN_LO, chain[0]);
		last_chain[0] = chain[0];
	}
	if (again || chain[1] != last_chain[1]) {
		send_midi_cc(MIDI_CC_STATUS_CHAIN_HI, chain[1]);
		last_chain[1] = chain[1];
	}

	for (int i = 0; i < ARRAY_SIZE(effects); i++)
		effects[i]->intense = 0;
	output_clipped = 0;
}

// 'update_ui()' is called every few ms to react to user events.
static void update_ui(void)
{
	static int effect_idx = 0;

	struct effect *effect = effects[effect_idx];

	// Stomp: enable/disable all effects
	if (switch_pressed(STOMP_SWITCH)) {
		switch_clear(STOMP_SWITCH);
		disable_all = EFF_ENABLE_STEPS * !disable_all;
		send_midi_cc(MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127);
	}

	//
	// Which effect the pedal itself is editing.  Nothing sets this
	// yet: the encoder that used to pick the effect is gone, and
	// what replaces it is bound up with making the stomp switch and
	// the rotary programmable.  So this is the hook for that, and
	// today it stays on effect 0.
	//
	int idx = current_midi_effect_idx;

	if (idx != effect_idx) {
		effect_idx = idx;
		effect = effects[idx];
	}

	show_status();

	if (attention_preview)
		attention_preview--;

	unsigned int seq = effect->seq;
	unsigned char *cur_pot = effect->pot_values[seq & 1];
	unsigned char *new_pot = effect->pot_values[!(seq & 1)];
	memcpy(new_pot, cur_pot, 10);

	// If something changed, let the other CPU know
	if (read_pots(effect, new_pot)) {
		for (int i=0; i<10; i++) {
			int val = new_pot[i];
			int old_val = cur_pot[i];
			if (val != old_val) {
				send_sysex_set_param(effect_idx, i+1, val);
			}
		}
		smp_store_release(&effect->seq, seq + 1);
	}

}
