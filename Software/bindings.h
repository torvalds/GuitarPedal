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
	ACT_POT,	// arg[0] = effect, arg[1] = pot, 0..9
	ACT_NEXT_POT,	// step whatever ACT_POT is pointing at
	ACT_BYPASS,
	ACT_TUNER,
	ACT_SCENE,	// arg[0] = scene
	NR_ACTIONS,
};

struct binding {
	unsigned char action;
	unsigned char arg[2];
};

//
// The defaults are the old hardcoded behaviour, spelled out.
//
// The rotary starts on the noise gate's first pot because that is where
// it always was - 'current_midi_effect_idx' was never assigned, so the
// pedal's own UI could only ever edit effect 0.
//
static struct binding bindings[NR_CONTROLS] = {
	[CTRL_ROTARY_TURN]	= { ACT_POT, { 0, 0 } },
	[CTRL_ROTARY_TAP]	= { ACT_NEXT_POT },
	[CTRL_ROTARY_HOLD]	= { ACT_NONE },
	[CTRL_STOMP_TAP]	= { ACT_BYPASS },
	[CTRL_STOMP_HOLD]	= { ACT_TUNER },
};

//
// Take a binding off the wire.
//
// Everything is range checked here rather than where it is used, so
// that the table can be trusted by the things that walk it.  An effect
// id out of range is an array read off the end, and the app is not the
// only thing that can send one of these.
//
// A pot with no label does not exist on that effect, which is the same
// test the UI uses to decide what to step past.
//
static bool set_binding(unsigned int ctrl, unsigned int action,
			unsigned int arg0, unsigned int arg1)
{
	if (ctrl >= NR_CONTROLS || action >= NR_ACTIONS)
		return false;

	switch (action) {
	case ACT_POT:
		if (arg0 >= ARRAY_SIZE(effects) || arg1 >= 10)
			return false;
		if (!effects[arg0]->pots[arg1].label)
			return false;
		break;
	case ACT_SCENE:
		if (arg0 >= nr_scenes)
			return false;
		break;
	default:
		// The arguments belong to the action, so an action that
		// takes none does not get to remember any.
		arg0 = 0;
		arg1 = 0;
		break;
	}

	bindings[ctrl].action = action;
	bindings[ctrl].arg[0] = arg0;
	bindings[ctrl].arg[1] = arg1;
	return true;
}

#endif
