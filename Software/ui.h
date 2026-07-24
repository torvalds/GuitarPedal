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

		// No rotary changes. Switch pressed?
		//
		// Clear and ignore long-press, it's the result
		// of "hold and rotate"
		unsigned int sw = __atomic_fetch_and(&switch_val, ~0x10001, __ATOMIC_RELAXED);
		sw &= 1;
		if (!sw)
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

static int switch_effect(int idx)
{
	// Ignore low two bits of rotary select
	int select = __atomic_fetch_and(&rotary_effect, 3, __ATOMIC_RELAXED);
	select &= ~3;
	if (!select)
		return idx;

	idx += (select < 0) ? -1 : 1;
	if (idx < 0)
		idx = 0;
	if (idx >= ARRAY_SIZE(effects))
		idx = ARRAY_SIZE(effects)-1;
	return idx;
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

// 'update_ui()' is called every few ms to react to user events.
static void update_ui(void)
{
	static int effect_idx = 0;
	static int last_active_pot = -1;

	struct effect *effect = effects[effect_idx];

	// Left stomp or bottom rotary long-press: save effect state to EEPROM
	if (switch_pressed(LONGPRESS(1)) || switch_pressed(LONGPRESS(3))) {
		switch_clear(LONGPRESS(1));
		switch_clear(LONGPRESS(3));
		unsigned int seq = effect->seq;
		unsigned char *cur_pot = effect->pot_values[seq & 1];
		unsigned char *new_pot = effect->pot_values[!(seq & 1)];
		memcpy(new_pot, cur_pot, 10);
		effect->seq = 0;

		save_effect_state(effect_idx, effect);
	}

	// Left stomp (or bottom rotary):
	// enable/disable current effect
	if (switch_pressed(1) || switch_pressed(3)) {
		switch_clear(1); switch_clear(3);
		effect->target = EFF_ENABLE_STEPS * !effect->target;
		send_sysex_set_param(effect_idx, 0, effect->target ? 127 : 0);
		for (int i=0; i<10; i++) {
			int val = effect->pot_values[effect->seq & 1][i];
			send_sysex_set_param(effect_idx, i+1, val);
		}
		last_effect = NULL;	// Force list_effects();
		effect->seq += 2;	// Force saving
	}

	// Right stomp: enable/disable all effects
	if (switch_pressed(2) || switch_pressed(4)) {
		switch_clear(2); switch_clear(4);
		disable_all = EFF_ENABLE_STEPS * !disable_all;
		send_midi_cc(MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127);
	}

	// Effect switching: lower rotary
	int idx = switch_effect(effect_idx);

	if (idx == effect_idx && current_midi_effect_idx != effect_idx) {
		idx = current_midi_effect_idx;
	}

	if (idx != effect_idx) {
		effect_idx = idx;
		effect = effects[idx];
		current_midi_effect_idx = idx;
		last_active_pot = -1; // Force active_pot update on screen switch

		send_midi_pc(effect_idx);
	}

	// The LED mappings have changed between boards
	bool led1 = !disable_all, led1_intense = clipping;
	bool led2 = effect->target, led2_intense = effect->intense;

#ifdef USB_MODE_HOST
	extern uint8_t remote_clipping;
	extern uint8_t remote_intense;
	led1_intense = remote_clipping;
	led2_intense = remote_intense;
#endif
	set_led(PWM_PIN1, led1, led1_intense);
	set_led(PWM_PIN2, led2, led2_intense);

	static uint8_t last_clipping = 0;
	static uint8_t last_intense = 0;
	if (clipping != last_clipping) {
		send_midi_cc(MIDI_CC_AUDIO_CLIPPING, clipping ? 127 : 0);
		last_clipping = clipping;
	}
	if (effect->intense != last_intense) {
		send_midi_cc(MIDI_CC_EFFECT_INTENSE, effect->intense ? 127 : 0);
		last_intense = effect->intense;
	}

	effect->intense = 0;
	clipping = 0;

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
		effect->seq++;
	}

	extern volatile bool ui_sync_changed;
	if (__atomic_exchange_n(&ui_sync_changed, false, __ATOMIC_RELAXED)) {
		last_effect = NULL;
	}

	if (effect->active_pot != last_active_pot) {
		send_midi_cc(MIDI_CC_ACTIVE_POT, effect->active_pot);
		last_active_pot = effect->active_pot;
	}
}
