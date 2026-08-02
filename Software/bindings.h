#ifndef BINDINGS_H
#define BINDINGS_H

//
// What the physical controls actually do.
//
// There is one rotary and one footswitch, and what a gesture on either
// of them means is looked up here rather than compiled in.  That is the
// whole point: a pedal with no screen cannot tell you what its knob is
// for, so the answer has to be set from something that can, and the
// WebMIDI app is that something.
//
// Nothing here is saved.  The table is rebuilt from the defaults below
// on every boot, and those defaults are exactly what the pedal did when
// the gestures were hardcoded, so an unprogrammed pedal behaves as it
// always has.  Persisting it waits for the storage rework, because the
// layout should not be guessed at before the shape of what is stored
// has settled - which is also what makes this testable on the old board
// with the 2kbit part, where there is nowhere to save it to anyway.
//

//
// The gestures - what a binding is *from*.
//
// This lists the gestures that exist, not the ones that are imaginable.
// The next board's jack carries either two more footswitches or an
// analog expression pedal, and whichever it turns out to be becomes
// more entries here; the wire format has a whole byte for the id, so
// growing the list costs nothing but the entries.
//
// Turning the rotary while pressing its shaft is deliberately not one
// of these.  It used to select a pot, and it cannot coexist with a long
// press on the same shaft, because holding the shaft in order to turn
// it manufactures one every time.
//
enum control_id {
	CTRL_ROTARY_TURN,
	CTRL_ROTARY_TAP,
	CTRL_ROTARY_HOLD,
	CTRL_STOMP_TAP,
	CTRL_STOMP_HOLD,
	NR_CONTROLS,
};

//
// ...and what a binding is *to*.
//
// ACT_POT is the only one that means anything for a control that turns,
// and the others are the only ones that mean anything for one that
// clicks.  Nothing here enforces that.  Binding the footswitch to a pot
// is useless rather than dangerous, and the place to make a pointless
// choice hard to express is the app, not the wire.
//
enum bind_action {
	ACT_NONE,
	ACT_POT,	// turn it: drive the target
	ACT_NEXT_POT,	// step the knob's pot to the next one
	ACT_RESET_POT,	// put the target back to its schema default
	ACT_SET_POT,	// set the target to val[0]
	ACT_TOGGLE_POT,	// flip the target between val[0] and val[1]
	ACT_BYPASS,
	ACT_TUNER,
	ACT_SCENE,	// effect = scene id
	NR_ACTIONS,
};

//
// A target of BIND_FOLLOW means "whatever the knob is turning", rather
// than a particular pot.
//
// It exists for the way back.  A press that resets the knob has to
// follow the knob when the knob is rebound, or it quietly goes on
// resetting the pot you moved away from - and the whole reason that
// press exists is to be the reliable way back when you cannot see
// anything.  Naming the pot twice would work right up until it did not.
//
// It is deliberately not offered for the actions that carry a value.
// A value only means something once the pot is known: 80 is unity on
// the master volume and an arbitrary number of milliseconds on a delay
// time.  If you want to name a value, name the pot it belongs to.
//
// 0x7f rather than 0xff because everything here goes out over SysEx,
// where the high bit is not ours to use.
//
#define BIND_FOLLOW 0x7f

//
// 'pot' numbers a parameter the way the SysEx parameter write already
// does: 0 is the mix, 1 to 10 are the effect's own pots.  Two
// conventions for "which parameter of which effect" in one firmware
// would be one too many, and the mix is worth having - toggling it
// between nothing and everything is how a footswitch turns a single
// effect on and off.
//
// 'control' is which gesture fires it, and the table is flat: a gesture
// may appear in it more than once, so one press can do several things.
// Toggling one effect's mix up while taking another's down is how you
// switch between two effects without leaving the scene, and that is not
// expressible at all when a gesture gets exactly one action.
//
// Flat rather than a list hanging off each control, because it makes
// the natural write "here is the whole table" instead of "insert into
// row K's list" - and the app already draws only what the pedal echoes
// back, so that makes both directions the same message and leaves no
// insert, delete or reorder protocol to get wrong.
//
struct rule {
	unsigned char control;
	unsigned char action;
	unsigned char effect;
	unsigned char pot;
	unsigned char val[2];
};

#define MAX_RULES 16

//
// Defaults for a pedal nobody has programmed.
//
// The rotary is the master volume, which is the only thing a single
// unlabelled knob can plausibly be.  It does not change what any effect
// hears - Trim and Volume are the two ends of the chain and this is the
// far one - so getting it wrong costs loudness rather than tone, and it
// is audible, which matters when there is nothing to look at.  It is
// also the parameter this firmware already treats as special: CC 7 goes
// straight to it.
//
// Both rotary presses reset it, and that is deliberate rather than a
// gesture going to waste.  switch_irq() sets either the short bit or
// the long one and never both, so a press held a moment too long
// arrives only as a long press.  If the two did different things, a
// slightly slow press would silently do the wrong one - and this action
// exists precisely to be the way back when you cannot see what you are
// doing, so it is the last thing that should be fussy about timing.
//
// The footswitch keeps what it always did.
//
static struct rule rules[MAX_RULES] = {
	{ CTRL_ROTARY_TURN, ACT_POT,       0, CHAIN_VOLUME + 1 },
	{ CTRL_ROTARY_TAP,  ACT_RESET_POT, BIND_FOLLOW },
	{ CTRL_ROTARY_HOLD, ACT_RESET_POT, BIND_FOLLOW },
	{ CTRL_STOMP_TAP,   ACT_BYPASS },
	{ CTRL_STOMP_HOLD,  ACT_TUNER },
};
static unsigned int nr_rules = 5;

//
// Does this action point at a pot, and may it say "the knob's one"?
//
static bool action_has_target(unsigned int action)
{
	return action == ACT_POT || action == ACT_RESET_POT ||
	       action == ACT_SET_POT || action == ACT_TOGGLE_POT;
}

static bool action_may_follow(unsigned int action)
{
	return action == ACT_RESET_POT;
}

//
// Take the whole table off the wire.
//
// Everything is range checked here rather than where it is used, so
// that the table can be trusted by the things that walk it.  An effect
// id out of range is an array read off the end, and the app is not the
// only thing that can send one of these.
//
// A rule that does not check out is dropped rather than the batch being
// rejected.  The pedal answers with what it kept, so a dropped rule
// shows up at once as a row that did not come back - which is a better
// way to be told than an error the app would have to render, and it
// stops one bad rule from losing the other fifteen.
//
static bool rule_ok(const struct rule *r)
{
	if (r->control >= NR_CONTROLS || r->action >= NR_ACTIONS)
		return false;

	if (r->action == ACT_SCENE)
		return r->effect < MAX_SCENES;

	if (!action_has_target(r->action))
		return true;

	if (r->effect == BIND_FOLLOW)
		return action_may_follow(r->action);

	if (r->effect >= ARRAY_SIZE(effects) || r->pot > 10)
		return false;

	//
	// An effect with no wet and no dry has no mix to bind to, and a
	// pot with no label does not exist on that effect.
	//
	if (!r->pot)
		return !effects[r->effect]->no_mix;
	return effects[r->effect]->pots[r->pot - 1].label != NULL;
}

static void set_rules(const uint8_t *buf, unsigned int count)
{
	unsigned int kept = 0;

	if (count > MAX_RULES)
		count = MAX_RULES;

	for (unsigned int i = 0; i < count; i++) {
		const uint8_t *p = buf + i * 6;
		struct rule r = { p[0], p[1], p[2], p[3], { p[4], p[5] } };

		//
		// Values are not checked.  A pot's range is a property
		// of the pot, and for a following target there is no pot
		// yet to ask - so they are clamped where they are used,
		// which is the only place that can always do it.
		//
		if (rule_ok(&r))
			rules[kept++] = r;
	}
	nr_rules = kept;
}

#endif
