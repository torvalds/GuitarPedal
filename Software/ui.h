//
// This is the "ui" for now - really just for very random testing
//
#include "eeprom.h"

static void move_pot(struct effect *effect, int dir)
{
	if (!dir)
		return;

	int new_active = effect->active_pot;
	do {
		new_active += dir;
		if (new_active < 0)
			new_active = 9;
		else if (new_active > 9)
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
//
// Also note how the 'select' rotary low bits are ignored but allowed
// to accumulate - but cleared if something else happens.
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
		// Ignore low two bits of rotary select
		int select = __atomic_fetch_and(&rotary_select, 3, __ATOMIC_RELAXED);
		select &= ~3;

		if (select) {
			int dir = (select < 0) ? -1 : 1;

			move_pot(effect, dir);
			return true;
		}

		// No rotary changes. Shaft pressed?
		//
		// Clear and ignore the long press, it's the result
		// of "hold and rotate"
		unsigned int both = (1u << ROTARY_SWITCH) | (1u << LONGPRESS(ROTARY_SWITCH));
		unsigned int sw = __atomic_fetch_and(&switch_val, ~both, __ATOMIC_RELAXED);
		if (!(sw & (1u << ROTARY_SWITCH)))
			return false;
		move_pot(effect, 1);
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

static void set_led(int pin, bool on, bool intense)
{
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
// Tell the host what the one LED can only hint at.  See midi.h for what
// goes in which bit.
//
// Only on change, which at this rate means a steady state costs nothing
// and anything new is on screen within 40ms.
//
// Clearing 'intense' for every effect rather than just the one being
// edited is what makes the chain bits mean anything.  The audio core
// sets them at 48kHz and this is the only thing that ever puts them
// back, so an effect that has stopped asking for attention would
// otherwise stay lit for good - which is exactly what boost, compressor
// and echo have been doing, unnoticed, because nothing read them.
//
// Draining 'samples_dropped' here is also why this has to come after
// set_led(): that reads it, and this is what clears it.
//
static void send_status(void)
{
	unsigned int dropped = __atomic_exchange_n(&samples_dropped, 0,
						   __ATOMIC_RELAXED);
	uint8_t global = dropped > STATUS_DROPPED_MASK
		       ? STATUS_DROPPED_MASK : dropped;
	uint8_t chain[2] = { 0, 0 };

	if (output_clipped)
		global |= STATUS_CLIPPED;
	if (effects[0]->intense)
		global |= STATUS_FRONT_ATTN;

	for (int i = 0; i < routed_effect_count; i++) {
		if (effects[effect_chain[i]]->intense)
			chain[i / STATUS_CHAIN_BITS] |= 1u << (i % STATUS_CHAIN_BITS);
	}

	static uint8_t last_global = 0;
	static uint8_t last_chain[2] = { 0, 0 };

	if (global != last_global) {
		send_midi_cc(MIDI_CC_STATUS_GLOBAL, global);
		last_global = global;
	}
	if (chain[0] != last_chain[0]) {
		send_midi_cc(MIDI_CC_STATUS_CHAIN_LO, chain[0]);
		last_chain[0] = chain[0];
	}
	if (chain[1] != last_chain[1]) {
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

	//
	// One LED: lit while the pedal is passing effects, bright when
	// something wants your attention.  See status.h for why all three
	// of those share the one brightness.
	//
	// 'samples_dropped' is still set at this point - send_status()
	// below is what drains it, and it reports the count.
	//
	set_led(LED_GPIO, !disable_all,
		output_clipped || samples_dropped || attention_preview);

	send_status();

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
