//
// This is the "ui" for now - really just for very random testing
//
#include "scene.h"

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
// Which pot a binding acts on.
//
// BIND_FOLLOW means the knob's, which can only be answered at the
// moment the gesture happens - that is the entire point of it, and why
// it is resolved here rather than stored.
//
//
// The rule the knob is driving, for the things that follow it.
//
// The first one, when there are several: a knob bound to two parameters
// at once is a perfectly good macro control, but "reset the knob's
// parameter" has to mean one parameter, and the first is the only
// answer that does not depend on how the list happens to be ordered
// later.
//
static struct rule *knob_rule(void)
{
	for (unsigned int i = 0; i < nr_rules; i++) {
		if (rules[i].control == CTRL_ROTARY_TURN &&
		    rules[i].action == ACT_POT)
			return &rules[i];
	}
	return NULL;
}

static struct effect *bind_target(const struct rule *b, unsigned int *pot,
				  unsigned int *eff_id)
{
	unsigned int eff = b->effect, idx = b->pot;

	if (eff == BIND_FOLLOW) {
		const struct rule *knob = knob_rule();

		if (!knob)
			return NULL;
		eff = knob->effect;
		idx = knob->pot;
	}

	if (eff >= ARRAY_SIZE(effects) || idx > 10)
		return NULL;
	if (!idx) {
		if (effects[eff]->no_mix)
			return NULL;
	} else if (!effects[eff]->pots[idx - 1].label) {
		return NULL;
	}

	*pot = idx;
	*eff_id = eff;
	return effects[eff];
}

//
// A target's current value, default and range, with the mix folded in
// as parameter 0 the way the wire numbers it.  The mix is a float on
// its own scale rather than a byte in pot_values[], which is the whole
// reason these three exist instead of the callers indexing directly.
//
static int target_value(struct effect *e, unsigned int pot)
{
	switch (pot) {
	case POT_MIX:		return FLOAT_TO_POT(e->mix_pot);
	case POT_CH_IN:		return CH_IN(e->channels);
	case POT_CH_OUT:	return CH_OUT(e->channels);
	case POT_MERGE:		return FLOAT_TO_POT(e->merge);
	}
	return e->pot_values[e->seq & 1][pot - 1];
}

//
// The defaults are the ones reset_effect() hands out: read the left
// channel, write both, and keep all of whatever was not touched - which
// is what every effect did before there was a choice.
//
static int target_default(struct effect *e, unsigned int pot)
{
	switch (pot) {
	case POT_MIX:		return FLOAT_TO_POT(e->def_mix);
	case POT_CH_IN:		return CH_IN_LEFT;
	case POT_CH_OUT:	return CH_OUT_BOTH;
	case POT_MERGE:		return 120;
	}
	return e->pots[pot - 1].def_val;
}

static struct pot_range target_range(struct effect *e, unsigned int pot)
{
	switch (pot) {
	case POT_MIX:		return (struct pot_range){ 0, 120 };
	case POT_CH_IN:		return (struct pot_range){ 0, CH_IN_RIGHT };
	case POT_CH_OUT:	return (struct pot_range){ 0, CH_OUT_MERGE };
	case POT_MERGE:		return (struct pot_range){ 0, 120 };
	}
	return get_pot_range(e->pots + pot - 1);
}

//
// Put a value on a bound pot, and tell the app.
//
// Clamped here rather than where the binding was accepted, because this
// is the only place that always knows which pot it is: a following
// target does not have one until the gesture happens.
//
static void set_target(struct effect *effect, unsigned int eff_id,
		       unsigned int pot, int val)
{
	const struct pot_range range = target_range(effect, pot);

	if (val < range.min)
		val = range.min;
	else if (val > range.max)
		val = range.max;

	if (val == target_value(effect, pot))
		return;

	if (pot > POT_LAST)
		set_effect_steering(effect, pot, val);
	else if (pot)
		set_effect_pot(effect, pot - 1, val);
	else
		set_effect_mix(effect, val);

	send_sysex_set_param(eff_id, pot, val);
}

//
// The rotary, turned.
//
// Every rule the knob has gets the same movement, so a knob bound to
// two parameters moves both - which is what a macro control is.  The
// accumulator is drained once, using the first rule's range to decide
// how coarse to be, because it is one physical movement and it cannot
// be spent twice.
//
// Note that the "__atomic" part isn't actually about SMP, just the
// interrupts.  The low bits are deliberately left behind rather than
// drained, so that sub-detent movement on a coarse pot adds up instead
// of being thrown away a click at a time.
//
static void rotary_turned(void)
{
	struct rule *first = knob_rule();
	unsigned int pot, eff_id;
	struct effect *effect;
	int ignore_low_bits = 0;

	if (!first || !(effect = bind_target(first, &pot, &eff_id))) {
		//
		// Nothing to drive.  Drop what has accumulated rather
		// than saving it up, or the first thing bound here
		// would jump by however far the knob was fiddled with
		// while it was pointing at nothing.
		//
		__atomic_store_n(&rotary_value, 0, __ATOMIC_RELAXED);
		return;
	}

	// For small ranges, don't make the rotary so twitchy
	const struct pot_range range = target_range(effect, pot);
	if (range.max - range.min < 25)
		ignore_low_bits = 2;

	int mask = (1 << ignore_low_bits)-1;
	int val = __atomic_fetch_and(&rotary_value, mask, __ATOMIC_RELAXED);
	val >>= ignore_low_bits;
	if (!val)
		return;

	for (unsigned int i = 0; i < nr_rules; i++) {
		struct rule *r = &rules[i];

		if (r->control != CTRL_ROTARY_TURN || r->action != ACT_POT)
			continue;
		effect = bind_target(r, &pot, &eff_id);
		if (!effect)
			continue;
		//
		// set_target() clamps and tells the app, and does
		// nothing when the value did not move - which is what
		// stops a knob held against an end from sending the
		// same number for ever.
		//
		set_target(effect, eff_id, pot,
			   target_value(effect, pot) + val);
	}
}

//
// Step the rotary's target to the next parameter that exists on the
// same effect, wrapping.  Parameter 0 is the mix, so an effect that has
// one is stepped through as well.
//
// Only ever forwards: going backwards was what hold-and-turn did, and
// that gesture is gone.
//
// This is the one action whose effect is to move another binding, so it
// is also the one that has to say so afterwards.
//
static void next_bound_pot(void)
{
	struct rule *b = knob_rule();

	if (!b || b->effect >= ARRAY_SIZE(effects))
		return;

	struct effect *effect = effects[b->effect];
	int idx = b->pot;

	do {
		if (++idx > 10)
			idx = 0;
		if (idx == b->pot)
			return;
	} while (idx ? !effect->pots[idx - 1].label : effect->no_mix);

	//
	// Deliberately on the resolved table rather than on whichever
	// level supplied the rule.  Stepping the knob to the next
	// parameter is something you do mid-song to pick what it
	// adjusts, not a change to how the pedal is set up - so it
	// lasts until the scene is reloaded and is not written to
	// flash, which is also what it did when there was only one
	// table and nothing was saved at all.
	//
	b->pot = idx;
	sysex_send_bindings(RULES_EFFECTIVE);
}

//
// The three press actions that put a value on a pot.
//
// The LED flashes whether or not anything moved.  A press that changes
// nothing - because you were already at the default, or because the
// binding points at nothing - still has to be distinguishable from a
// press that did not register.  "Did that do anything?" is the question
// these exist to stop you having to ask, so they cannot be silent.
//
static void do_pot_action(const struct rule *b)
{
	unsigned int pot, eff_id;
	struct effect *effect = bind_target(b, &pot, &eff_id);

	if (effect) {
		int val = -1;

		switch (b->action) {
		case ACT_RESET_POT:
			val = target_default(effect, pot);
			break;
		case ACT_SET_POT:
			val = b->val[0];
			break;
		case ACT_TOGGLE_POT:
			//
			// Stateless: whichever of the two it is not on
			// right now.  Remembering which one it went to
			// last would be one more thing to fall out of
			// step with a value the app has since changed.
			//
			val = target_value(effect, pot) == b->val[0]
			    ? b->val[1] : b->val[0];
			break;
		}

		if (val >= 0)
			set_target(effect, eff_id, pot, val);
	}

	attention_preview = ATTENTION_PREVIEW_TICKS;
}

//
// A scene change asked for by a rule, and not yet done.  -1 for none.
//
static int pending_scene = -1;

static void do_rule(const struct rule *b)
{
	switch (b->action) {
	case ACT_NEXT_POT:
		next_bound_pot();
		break;

	case ACT_RESET_POT:
	case ACT_SET_POT:
	case ACT_TOGGLE_POT:
		do_pot_action(b);
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
		// Noted, not done.  Loading a scene rebuilds the rule
		// table - resolve_rules() - and 'b' points into that
		// table, which fire_control() is in the middle of
		// walking.  Switching here would pull the list out from
		// under the loop that is reading it, and the rest of
		// the gesture's rules would come from whichever table
		// replaced it.
		//
		// So the switch waits until nothing is iterating.  It
		// also means a gesture bound to two scene changes picks
		// the last one rather than loading both, which is a more
		// sensible answer than either.
		//
		// Checked again rather than trusted from the table.
		// set_binding() saw the same MAX_SCENES, and this costs
		// nothing.
		//
		if (b->effect < MAX_SCENES)
			pending_scene = b->effect;
		break;
	}
}

//
// Every rule that names this gesture, in table order.
//
// They are not atomic against the audio core: each one publishes as it
// goes, so core 1 can see one parameter moved and the next not yet.
// That is a sample or two apart on a 25Hz tick, and every one of these
// ends up crossfaded by EFF_ENABLE_STEPS anyway, so it is not audible
// and not worth a second publishing mechanism to avoid.
//
static void fire_control(unsigned int ctrl)
{
	for (unsigned int i = 0; i < nr_rules; i++) {
		if (rules[i].control == ctrl)
			do_rule(&rules[i]);
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
		fire_control(gestures[i].ctrl);
	}

	//
	// Out here, where the rule table is not being walked.
	//
	// Everything moves: pots, routing, and the rules themselves, so
	// whatever the app has cached is wrong in every particular.  Ask
	// for the whole dump rather than working out what changed - this
	// happens when somebody steps on a switch, not per sample.
	//
	if (pending_scene >= 0) {
		//
		// Only to a scene that has something in it.  load_scene()
		// on an empty one resets every effect and routes nothing,
		// so a rule aimed at a scene nobody has saved would take
		// a working pedal to silence at the touch of a switch -
		// and the way back would be the switch that just did it,
		// which now belongs to whatever the defaults say.
		//
		// Refusing leaves you where you were, which is wrong in a
		// way you can hear and recover from.
		//
		if (populated_scenes() & (1u << pending_scene)) {
			load_scene(pending_scene);
			state_dump_tx = true;
		}
		pending_scene = -1;
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
