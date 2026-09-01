#ifndef EFFECT_STATE_H
#define EFFECT_STATE_H

//
// How core 0 changes an effect.
//
// Two halves of one idea.  Which effects are in the chain and in what
// order - built out of a bitmask so that routing something twice, or
// routing something that is not routable, cannot be expressed.  And how
// a single value reaches an effect that core 1 is stepping right now,
// which is the double-buffer dance: fill the row that is not live,
// publish it by releasing 'seq'.
//
// They belong together because they are the same rule seen twice.  Core
// 1 owns an effect while it is running, so core 0 never writes anything
// core 1 might be reading - it writes somewhere else and then says so.
// Routing an effect in or out is that at a coarser grain.
//
// Not in audio/effect.h, which is what core 1 reads: this is the other
// side of that boundary and none of it runs on the audio core.
//


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
	unsigned char *new_pot = effect_spare_pots(eff, seq);

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

	effect_publish(eff, seq);
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

//
// Everything that can go in the chain: all of them but the signal
// chain, which always runs first and outside it, and anything kept
// once rather than per scene - something stored once cannot be part of
// an arrangement that is stored per scene.
//
// GLOBAL_EFFECTS comes from the headers rather than from a position in
// this array.  It used to be "the last one", which was true of the only
// one there was and would have been quietly wrong for the second.
//
#define ALL_EFFECTS ((routing_bitmap_t)((1u << EFFECT_COUNT) - 1))
#define ROUTABLE_EFFECTS (ALL_EFFECTS & ~((routing_bitmap_t)1 | GLOBAL_EFFECTS))

#define effect_is_global(i) (((GLOBAL_EFFECTS) >> (i)) & 1)

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
	unsigned char *cur_pot = effect_pots_at(e, seq);
	unsigned char *new_pot = effect_spare_pots(e, seq);

	memcpy(new_pot, cur_pot, 10);
	new_pot[pot_idx] = val;
	effect_publish(e, seq);
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

#endif
