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

static char *to_ascii(unsigned char term, uint32_t val, char *p, int digits, int decimals)
{
	digits += decimals;
	*--p = term;
	do {
		*--p = '0' + val % 10;
		val /= 10;
		if (!--decimals)
			*--p = '.';
		digits--;
	} while (val || digits > 0);
	return p;
}

//
// Helper wrapper for printing floats. The 'places' thing
// is very much just approximate, and it's really a mix of
// 'significant digits' and 'places' depending on what you
// are printing out (ie it will limit to both).
//
// So printing out -12345678.9 to three "places" will result
// in ten bytes ("-12300000"), while printing out 0.0099 will
// result in "0.010"
//
static char *float_to_ascii(float val, int places)
{
	unsigned int target = 1;
	for (int i = 1; i < places; i++)
		target *= 10;

	float abs_val = fabsf(val);
	unsigned int decimals = 0, int_val;
	int_val = lrintf(abs_val);

	// Do we need to remove precision or add decimals?
	if (int_val > 10*target) {
		int remove = 0;
		do {
			remove++;
			abs_val /= 10;
			int_val = lrintf(abs_val);
		} while (int_val > 10*target);
		do { int_val *= 10; } while (--remove);
	} else {
		for (decimals = 0; int_val < target; decimals++) {
			if (decimals >= places)
				break;
			abs_val *= 10;
			int_val = lrintf(abs_val);
		}
	}

	// Old-school interfaces. We're not re-entrant
	static char internal_buffer[16];
	char *p = internal_buffer + sizeof(internal_buffer);

	p = to_ascii(0, int_val, p, 1, decimals);
	if (val < 0)
		*--p = '-';
	return p;
}

static void list_effects(struct effect *active)
{
	int x = 3;
	int y = 116;

	int pos = 0, mid = 0;
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		const char *name = effect->short_name;
		int width = strlen(name)*6 + 6;

		if (effect == active)
			mid = pos + width / 2;
		pos += width;
	}

	if (mid > 64) {
		int diff = mid - 64;
		if (pos - diff < 128)
			diff = pos - 128;
		x -= diff;
	}

	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		const char *name = effect->short_name;
		int width = strlen(name)*6;

		if (effect == active)
			sh1106_rectangle(x-3, y-3, width+6, 8+6, rect_border);
		sh1106_puts_6x8(x,y,name);
		if (effect->target)
			sh1106_reverse(x-1, y-1, width+2, 10);
		x += width+6;
	}
}

static void list_enums(const struct pot_descr *pot, int active_val, int y)
{
	int x = 3;
	int pos = 0, mid = 0;
	int count = 0;

	sh1106_clear(0,y,128,11);

	while (pot->enum_names[count]) {
		const char *name = pot->enum_names[count];
		int width = strlen(name)*6 + 6;
		if (count == active_val)
			mid = pos + width / 2;
		pos += width;
		count++;
	}

	if (pos < 128) {
		x = (128 - pos) / 2;
	} else if (mid > 64) {
		int diff = mid - 64;
		if (pos - diff < 128)
			diff = pos - 128;
		x -= diff;
	}

	for (int i = 0; i < count; i++) {
		const char *name = pot->enum_names[i];
		int width = strlen(name)*6;

		sh1106_puts_6x8(x,y+1,name);
		if (i == active_val)
			sh1106_reverse(x-1, y, width+2, 10);
		x += width+6;
	}
}

static void pot_describe(const struct pot_descr *pot, int val, int posY)
{
	// 8 characters for label
	// 7 characters for numerical value
	// 5 characters for units
	char desc[22] = "                     ";
	char *end = desc+16;
	int decimals = 0;

	if (pot->unit) {
		int len = strlen(pot->unit);
		if (len > 5) len = 5;
		memcpy(end, pot->unit, len);
	}

	if (pot->enum_names) {
		int e_val = val;
		if (e_val < 0) e_val = 0;
		const char *name = pot->enum_names[e_val];
		if (name) {
			int len = strlen(name);
			if (len > 11) len = 11;
			memcpy(desc + 10, name, len);
		}
	} else {
		if (pot->convert) {
			float fval = pot->convert(val);
			if (fabsf(fval) < 100) {
				fval *= 10;
				decimals = 1;
				if (fabsf(fval) < 100) {
					fval *= 10;
					decimals = 2;
				}
			}
			val = lrintf(fval);
			if (decimals > 1 && !(val % 100)) {
				end--;
				decimals--;
				val /= 10;
			}
		}
		end = to_ascii(' ', abs(val), end, 1, decimals);
		if (val < 0)
			*--end = '-';
	}

	const char *label = pot->label;
	int len = strlen(label);
	if (len > 8) len = 8;
	memcpy(desc, label, len);
	desc[len] = ':';
	sh1106_puts_6x8(10, posY, desc);
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
static void update_ui(int force_update)
{
	static int effect_idx = 0;
	static int last_active_pot = -1;

	struct effect *effect = effects[effect_idx];
	bool update_screen = false;

	if (force_update) {
		last_effect = NULL;
		update_screen = true;
	}

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
		update_screen = true;
	}

	// Left stomp (or bottom rotary):
	// enable/disable current effect
	if (switch_pressed(1) || switch_pressed(3)) {
		switch_clear(1); switch_clear(3);
		effect->target = EFF_ENABLE_STEPS * !effect->target;
		uint8_t en_cc = effect_enable_to_cc[effect_idx];
		if (en_cc) send_midi_cc(en_cc, effect->target ? 127 : 0);
		for (int i=0; i<10; i++) {
			uint8_t cc = effect_pot_to_cc[effect_idx][i];
			if (cc) {
				int val = effect->pot_values[effect->seq & 1][i];
				uint8_t midi_val = val;
				send_midi_cc(cc, midi_val);
			}
		}
		update_screen = true;
		last_effect = NULL;	// Force list_effects();
		effect->seq += 2;	// Force saving
	}

	// Right stomp: enable/disable all effects
	if (switch_pressed(2)) {
		switch_clear(2);
		disable_all = EFF_ENABLE_STEPS * !disable_all;
		send_midi_cc(GLOBAL_ENABLE_CC, disable_all ? 0 : 127);
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

		update_screen = true;
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
			uint8_t cc = effect_pot_to_cc[effect_idx][i];
			if (cc && val != old_val) {
				uint8_t midi_val = val;
				send_midi_cc(cc, midi_val);
			}
		}
		effect->seq++;
		update_screen = true;
	}

	extern volatile bool ui_sync_changed;
	if (__atomic_exchange_n(&ui_sync_changed, false, __ATOMIC_RELAXED)) {
		last_effect = NULL;
		update_screen = true;
	}

	if (effect != last_effect) {
		last_effect = effect;
		sh1106_clear(0, 0, 128, 128);
		sh1106_puts_8x16(10, 70, effect->name);
		list_effects(effect);
		update_screen = true;
	}

	if (effect->active_pot != last_active_pot) {
		send_midi_cc(MIDI_CC_ACTIVE_POT, effect->active_pot);
		last_active_pot = effect->active_pot;
	}

#if 0
	char buffer[16], *end = buffer+sizeof(buffer);

	unsigned int tx = i2s_dma_tx_ptr() - i2s_dma_buf;
	unsigned int rx = i2s_dma_rx_ptr() - i2s_dma_buf;
	unsigned int cpu = cpu_idx;

	char *p = to_ascii(0, (rx - tx) & 31, end, 1, 0);
	sh1106_puts_6x8(126-6*(end-p-1),64,p);
	p = to_ascii(0, (cpu - tx) & 31, end, 1, 0);
	sh1106_puts_6x8(126-6*(end-p-1),72,p);
	update_screen = true;
#endif

	if (!update_screen)
		return;

	int active = effect->active_pot;
	if (effect->graph) {
		effect->graph(effect, active, new_pot);
	} else {
		int total_pots = 0;
		while (total_pots < 10 && effect->pots[total_pots].label)
			total_pots++;

		int start_pot = active - 2;
		if (start_pot + 5 > total_pots)
			start_pot = total_pots - 5;
		if (start_pot < 0)
			start_pot = 0;

		for (int i = 0; i < 5; i++) {
			int pot_idx = start_pot + i;
			if (pot_idx >= total_pots)
				break;
			const struct pot_descr *pot = effect->pots + pot_idx;
			pot_describe(pot, new_pot[pot_idx], 12*i);
			sh1106_puts_6x8(0,12*i, (pot_idx == active) ? ">" : " ");
		}
	}

	const struct pot_descr *pot = effect->pots + active;
	const char *label = pot->label;
	if (label) {
		int val = new_pot[active];
		int posY = 100;

		pot_describe(pot, val, 90);

		if (pot->enum_names) {
			list_enums(pot, val, posY);
		} else {
			struct pot_range range = get_pot_range(pot);

			int half = (range.max - range.min)/2;

			int x = 64 + 64 * (val - range.min - half) / half;
			int def_x = 64 + 64 * (pot->def_val - range.min - half) / half;

			sh1106_rectangle(0,posY,128,11,rect_clear);
			sh1106_rectangle(def_x,posY,1,11,rect_lines);
			sh1106_rectangle(x-3,posY,7,11,rect_lines);
		}
	}

	sh1106_draw();
}
