//
// This is the "ui" for now - really just for very random testing
//
#include "eeprom.h"

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

//
// The rotary, turned.
//
// Note that the "__atomic" part isn't actually about SMP, just the
// interrupts.  The low bits of the accumulator are deliberately left
// behind rather than drained, so that sub-detent movement on a coarse
// pot adds up instead of being thrown away a click at a time.
//
static void rotary_turned(void)
{
	const struct binding *b = &bindings[CTRL_ROTARY_TURN];

	if (b->action != ACT_POT) {
		//
		// Nothing to drive.  Drop what has accumulated rather
		// than saving it up, or the first thing bound here
		// would jump by however far the knob was fiddled with
		// while it was pointing at nothing.
		//
		__atomic_store_n(&rotary_value, 0, __ATOMIC_RELAXED);
		return;
	}

	struct effect *effect = effects[b->arg[0]];
	unsigned int idx = b->arg[1];
	const struct pot_range range = get_pot_range(effect->pots + idx);

	// For small ranges, don't make the rotary so twitchy
	int ignore_low_bits = 0;
	if (range.max - range.min < 25)
		ignore_low_bits = 2;

	int mask = (1 << ignore_low_bits)-1;
	int val = __atomic_fetch_and(&rotary_value, mask, __ATOMIC_RELAXED);
	val >>= ignore_low_bits;
	if (!val)
		return;

	unsigned char *cur_pot = effect->pot_values[effect->seq & 1];
	val += cur_pot[idx];
	if (val < range.min)
		val = range.min;
	else if (val > range.max)
		val = range.max;

	if (val == cur_pot[idx])
		return;

	set_effect_pot(effect, idx, val);

	//
	// Tell the app.  It is only a notification - the pedal has
	// already made the change and would have made it with nothing
	// connected at all, which is the entire point of having a knob.
	//
	send_sysex_set_param(b->arg[0], idx + 1, val);
}

//
// Step the rotary's target to the next pot that exists on the same
// effect, wrapping.  Only ever forwards: going backwards was what
// hold-and-turn did, and that gesture is gone.
//
// This is the one action whose effect is to move another binding, so it
// is also the one that has to say so afterwards.
//
static void next_bound_pot(void)
{
	struct binding *b = &bindings[CTRL_ROTARY_TURN];

	if (b->action != ACT_POT)
		return;

	struct effect *effect = effects[b->arg[0]];
	int idx = b->arg[1];

	do {
		if (++idx > 9)
			idx = 0;
		if (idx == b->arg[1])
			return;
	} while (!effect->pots[idx].label);

	b->arg[1] = idx;

	//
	// Keep 'active_pot' meaning what it has always meant: the pot
	// the rotary is pointing at.  settings.h reads it to decide
	// whether to preview the attention brightness while you are
	// setting it, and that has never once been true - until now the
	// rotary could only ever be on effect 0.
	//
	effect->active_pot = idx;

	sysex_send_bindings();
}

//
// Put the rotary's pot back where it started.
//
// The LED says so whether or not anything moved.  A press that changes
// nothing because you were already at the default still has to be
// distinguishable from a press that did not register - "did that do
// anything?" is exactly the question this action exists to stop you
// having to ask, so it cannot itself be silent.
//
static void reset_bound_pot(void)
{
	struct binding *b = &bindings[CTRL_ROTARY_TURN];

	if (b->action == ACT_POT) {
		struct effect *effect = effects[b->arg[0]];
		unsigned int idx = b->arg[1];
		unsigned char def = effect->pots[idx].def_val;

		if (effect->pot_values[effect->seq & 1][idx] != def) {
			set_effect_pot(effect, idx, def);
			send_sysex_set_param(b->arg[0], idx + 1, def);
		}
	}

	attention_preview = ATTENTION_PREVIEW_TICKS;
}

static void do_binding(const struct binding *b)
{
	switch (b->action) {
	case ACT_NEXT_POT:
		next_bound_pot();
		break;

	case ACT_RESET_POT:
		reset_bound_pot();
		break;

	case ACT_BYPASS:
		disable_all = EFF_ENABLE_STEPS * !disable_all;
		send_midi_cc(MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127);
		break;

	case ACT_TUNER:
		tuner_mode = !tuner_mode;
		send_midi_cc(MIDI_CC_GLOBAL_ENABLE, tuner_mode ? 68 : 69);
		break;

	case ACT_SCENE:
		//
		// Checked again rather than trusted from the table.
		// set_binding() saw the same 'nr_scenes', but it is
		// decided by probing the board at startup and this
		// costs nothing.
		//
		if (b->arg[0] < nr_scenes)
			load_scene(b->arg[0]);
		break;
	}
}

//
// Whatever the switches are bound to.
//
// Called from the main loop ahead of the tuner-mode check rather than
// from update_ui(), which does not run in tuner mode - something has to
// be able to turn it off again.
//
static void handle_switch_bindings(void)
{
	static const struct {
		unsigned char sw;
		unsigned char ctrl;
	} gestures[] = {
		{ ROTARY_SWITCH,		CTRL_ROTARY_TAP },
		{ LONGPRESS(ROTARY_SWITCH),	CTRL_ROTARY_HOLD },
		{ STOMP_SWITCH,			CTRL_STOMP_TAP },
		{ LONGPRESS(STOMP_SWITCH),	CTRL_STOMP_HOLD },
	};

	for (int i = 0; i < ARRAY_SIZE(gestures); i++) {
		if (!switch_pressed(gestures[i].sw))
			continue;
		switch_clear(gestures[i].sw);
		do_binding(&bindings[gestures[i].ctrl]);
	}
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

//
// 'update_ui()' is called every few ms to react to user events.
//
// The switches are not read here - see handle_switch_bindings(), which
// the main loop calls before deciding whether it is in tuner mode.
//
static void update_ui(void)
{
	show_status();

	if (attention_preview)
		attention_preview--;

	rotary_turned();
}
