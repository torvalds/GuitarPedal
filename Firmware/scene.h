//
// Scenes, in the save area.
//
// What actually goes in the slots that flash_store.h hands out.  A
// scene is one key; the pedal's global settings are another.  Reading
// one is reading a pointer into flash and copying values out of it, and
// writing one is filling the staging buffer and committing it.
//
// This replaces eeprom.h, which had to fit a scene into 256 bytes and
// bent itself into interesting shapes doing it - the chain order was a
// linked list threaded through a spare byte of each effect's slot,
// because there was nowhere else to put it.  There is 4032 bytes here
// and a scene needs about 800, so nothing has to be clever.
//
//
// Surviving the effect table changing.
//
// The hard part is not storing values, it is knowing whether stored
// values still mean anything.  Effect ids are positional -
// gen_effects.py assigns them by (priority, base) - so adding one
// effect renumbers everything after it, and a scene that stored ids
// would quietly route the wrong things.
//
// So a scene does not store ids.  It stores, per effect, the two
// constants from gen_effects.py: 'id_hash' saying which effect it was,
// and 'pot_hash' saying what that effect's pots meant at the time.  The
// routing is a list of indices into the scene's *own* list, so it does
// not depend on this build's numbering either.  A scene is therefore
// self-describing, and one saved before an effect was added still loads
// correctly afterwards.
//
// What happens when they do not match is graded rather than
// all-or-nothing, which is the other thing eeprom.h got wrong - one bad
// checksum there threw away the effect's whole state:
//
//	id_hash not found	the effect is gone.  Drop the entry, and
//				anything routing it.
//	pot_hash differs	same effect, different pots.  Keep it in
//				the chain, put its pots back to defaults.
//	both match		use the stored values.
//
//
// What is not in here.
//
// Integrity.  flash_store.h already hashes the whole slot with SHA-256
// and will not hand back a copy that fails, so nothing below needs a
// checksum of its own.  The two hashes above are about *meaning* - "are
// these numbers still about the thing I think they are" - which is a
// different question that no amount of CRC answers.
//
#ifndef SCENE_H
#define SCENE_H

//
// Eight, not the thirty-two the eeprom had.
//
// Thirty-two is where this can go; eight is what is turned on.  Eight
// scenes and one globals key is nine slots of sixty-four, which leaves
// fifty-five for rotation *and* for whatever turns out to want saving
// next.  The region is an eighth of the flash and is not going to grow,
// so the room has to come from not spending it in advance.
//
// Ahead of the includes because bindings.h wants it - a rule that
// changes scene has to be checked against how many there are - while
// the payload below wants bindings.h's 'struct rule'.  One of the two
// has to go first, and a constant has no dependencies of its own.
//
#define MAX_SCENES 8

#include "bindings.h"
#include "flash_store.h"

//
// Room for more effects than exist, so that adding one is not a format
// change.  Eighteen today.
//
#define SCENE_MAX_EFFECTS 32

_Static_assert(EFFECT_COUNT <= SCENE_MAX_EFFECTS,
	       "more effects than a scene can hold");

#define SCENE_VERSION 3

//
// Keys are 'kind << 8 | index', so a new kind of saved thing costs
// nothing to add later.
//
#define SCENE_KEY(n)	(0x0000 + (n))
#define GLOBALS_KEY	0x0100

struct scene_effect {
	uint32_t id_hash;
	uint32_t pot_hash;
	uint8_t pots[10];
	uint8_t mix;
	uint8_t channels;	// which channels it reads and writes
	uint8_t merge;		// how much of the kept channel a merge keeps
	uint8_t pad;
};

_Static_assert(sizeof(struct scene_effect) == 24, "scene_effect is 24 bytes");

struct scene_payload {
	uint16_t version;
	uint8_t nr_effects;
	uint8_t nr_routed;
	uint8_t routing[MAX_ROUTED_EFFECTS];	// indices into effects[]
	uint8_t nr_rules;
	uint8_t pad[3];
	struct scene_effect effects[SCENE_MAX_EFFECTS];
	struct rule rules[MAX_RULES];
};

struct globals_payload {
	uint16_t version;
	uint8_t nr_rules;
	uint8_t pad;
	struct scene_effect settings;

	//
	// Which effect each rule target index means, and nothing else -
	// id hashes without the pot values a scene stores beside them.
	// Global rules have no scene to index into and would otherwise
	// be the one thing left naming effects by a number that moves.
	// 128 bytes.
	//
	uint32_t effect_ids[SCENE_MAX_EFFECTS];

	struct rule rules[MAX_RULES];
};

//
// On-flash layout, so the offsets are asserted rather than assumed.
//
// Both structs carry uint32_t and so are 4-aligned, which means the
// compiler pads them in two places that reading the declaration does
// not show: two bytes at the end of every scene_effect, and two more
// before the array of them starts.  tools/scenebuild writes these from
// python and got both wrong first time.
//
_Static_assert(offsetof(struct scene_payload, routing) == 4, "routing at 4");
_Static_assert(offsetof(struct scene_payload, nr_rules) == 18, "nr_rules at 18");
_Static_assert(offsetof(struct scene_payload, effects) == 24, "effects at 24");
_Static_assert(offsetof(struct scene_payload, rules) == 24 + 32 * 24,
	       "rules follow the effects");

_Static_assert(sizeof(struct scene_payload) <= SAVE_PAYLOAD_SIZE,
	       "a scene does not fit in a slot");
_Static_assert(sizeof(struct globals_payload) <= SAVE_PAYLOAD_SIZE,
	       "the globals do not fit in a slot");

static uint8_t current_scene_id = 0;

//
// Which effect this stored entry is about, or NULL.
//
// A search rather than an index, which is the whole point: it asks
// "which effect calls itself this" instead of "what is at position N",
// and position N is exactly what moves.
//
static struct effect *scene_find_effect(const struct scene_effect *saved)
{
	for (int i = 0; i < EFFECT_COUNT; i++) {
		if (effects[i]->id_hash == saved->id_hash)
			return effects[i];
	}
	return NULL;
}

//
// Copy one effect's stored values into it.
//
// Refuses a pot that is out of range for what that pot is now, which
// mostly cannot happen - pot_hash has already said the layout matches -
// but a slot can also be planted by hand, and a value past the end of
// an enumeration would index a NULL name.
//
static void scene_load_effect(struct effect *eff, const struct scene_effect *saved)
{
	if (eff->pot_hash != saved->pot_hash)
		return;

	for (int i = 0; i < 10; i++) {
		if (saved->pots[i] > max_pot_val(eff, i))
			return;
	}
	if (saved->mix > 120)
		return;

	memcpy(eff->pot_values[0], saved->pots, 10);
	memcpy(eff->pot_values[1], saved->pots, 10);
	set_mix_pot(eff, POT_TO_FLOAT(saved->mix));
	eff->channels = saved->channels;
	eff->merge = POT_TO_FLOAT(saved->merge);
	eff->target = EFF_ENABLE_STEPS;
	eff->mix = eff->target;
	if (eff->init)
		eff->init(eff->pot_values[0]);
}

//
// Saving reads the *live* half, and reads it without a barrier.
//
// Both of those want saying, because everything else that touches
// pot_values[] is careful and this deliberately is not.
//
// The live half is the right one because it is what the pedal is
// currently making a noise with, and that is what somebody pressing save
// means.  The spare half is either a copy of it or a write that has not
// been published yet - neither is the sound in the room.
//
// And no barrier is needed because there is no race to lose.  Core 0 is
// the only writer of pot_values[], and this runs on core 0: the live
// half cannot change underneath it, because changing it is something
// this same core would have to do.  Core 1 reads the same bytes at the
// same time, which is two readers and no writer.
//
// What could move is which half is live, if a write landed between
// reading 'seq' and the memcpy - and it cannot, for the same reason.
//
static void scene_save_effect(struct effect *eff, struct scene_effect *out)
{
	out->id_hash = eff->id_hash;
	out->pot_hash = eff->pot_hash;
	memcpy(out->pots, effect_pots(eff), 10);
	out->mix = FLOAT_TO_POT(eff->mix_pot);
	out->channels = eff->channels;
	out->merge = FLOAT_TO_POT(eff->merge);
	out->pad = 0;
}

//
// Translate one level's stored rules into live ones.
//
// A rule's 'effect' field is three things depending on the action: an
// effect for ACT_POT and its relatives, a scene for ACT_SCENE, and
// BIND_FOLLOW meaning "whatever the knob is on".  Only the first is a
// number that moves, so only the first goes through the remap.
//
// 'remap' is stored index -> current effect id, or 0xFF for an effect
// that is no longer here.  A rule pointing at one of those is dropped:
// its target does not exist, and guessing a different one would be
// worse than the gesture doing nothing.
//
static void scene_load_rules(struct rule *dst, unsigned int *dst_count,
			     const struct rule *src, unsigned int count,
			     const uint8_t *remap)
{
	unsigned int kept = 0;

	if (count > MAX_RULES)
		count = MAX_RULES;

	for (unsigned int i = 0; i < count; i++) {
		struct rule r = src[i];

		if (action_has_target(r.action) && r.effect != BIND_FOLLOW) {
			if (r.effect >= SCENE_MAX_EFFECTS)
				continue;
			if (remap[r.effect] == 0xFF)
				continue;
			r.effect = remap[r.effect];
		}

		//
		// Checked after remapping, not before: rule_ok() asks
		// whether the pot exists on that effect, and which
		// effect it is has only just been decided.
		//
		if (rule_ok(&r))
			dst[kept++] = r;
	}
	*dst_count = kept;
}

//
// Load a scene, or leave everything at its defaults if there is none.
//
// An unsaved scene is not an error and has no fallback - the same
// answer eeprom.h gave, and for the same reason: there is nothing to
// fall back to, and a pedal with every effect at its default and
// nothing routed is a perfectly good starting point.
//
static bool load_scene(uint8_t scene_id)
{
	const struct scene_payload *scene;
	routing_bitmap_t routable;

	if (scene_id >= MAX_SCENES)
		return false;

	current_scene_id = scene_id;

	//
	// Back to defaults first, so an effect the scene does not
	// mention is at its defaults rather than at whatever the last
	// scene left it - except the settings, which are the pedal's and
	// not the song's.  Resetting those here would undo load_globals()
	// every time a scene loaded, which is a slower way of making them
	// per-scene again.
	//
	for (int i = 0; i < EFFECT_COUNT; i++) {
		if (effects[i] == &settings_effect)
			continue;
		reset_effect(effects[i]);
	}

	//
	// Cleared here, and resolved on the way out of every path
	// including the ones that give up below.  A scene with nothing
	// in it has no rules of its own and inherits, which is the same
	// thing a scene that was never saved should do - and getting
	// that wrong would leave a new pedal with an empty resolved
	// table and no working controls at all.
	//
	nr_scene_rules = 0;

	scene = save_read(SCENE_KEY(scene_id));
	if (!scene || scene->version != SCENE_VERSION ||
	    scene->nr_effects > SCENE_MAX_EFFECTS) {
		resolve_rules();
		return false;
	}

	//
	// The values first, then the chain.  An effect whose entry is
	// dropped keeps the defaults reset_effect() just gave it, so
	// "the effect moved on" and "the effect was never saved" arrive
	// at the same place.
	//
	uint8_t remap[SCENE_MAX_EFFECTS];

	memset(remap, 0xFF, sizeof(remap));
	for (int i = 0; i < scene->nr_effects; i++) {
		struct effect *eff = scene_find_effect(&scene->effects[i]);

		if (!eff)
			continue;

		//
		// Not the settings.  They are the pedal's rather than the
		// song's and live under their own key - see load_globals()
		// - and a scene that restored them would put the MIDI
		// channel and the USB routing back to whatever they were
		// when it was saved, which is the thing making them global
		// was meant to stop.
		//
		// Skipped here as well as left out of save_scene(), so a
		// scene written before that stops being true does not get
		// to apply what it kept.
		//
		if (eff == &settings_effect)
			continue;
		scene_load_effect(eff, &scene->effects[i]);
		for (int n = 0; n < EFFECT_COUNT; n++) {
			if (effects[n] == eff) {
				remap[i] = n;
				break;
			}
		}
	}

	routable = routing_start();
	for (int i = 0; i < scene->nr_routed && i < MAX_ROUTED_EFFECTS; i++) {
		uint8_t idx = scene->routing[i];

		if (idx < SCENE_MAX_EFFECTS && remap[idx] != 0xFF)
			routing_add(&routable, remap[idx]);
	}
	routing_end(routable);

	scene_load_rules(scene_rules, &nr_scene_rules,
			 scene->rules, scene->nr_rules, remap);
	resolve_rules();

	return true;
}

static bool save_scene(uint8_t scene_id)
{
	struct scene_payload *scene;

	if (scene_id >= MAX_SCENES)
		return false;

	current_scene_id = scene_id;

	scene = (struct scene_payload *)save_buffer();
	scene->version = SCENE_VERSION;

	//
	// Every effect, routed or not, so that the entry list is a
	// stable thing for the routing to index into and an effect that
	// is not in the chain still has somewhere to keep its values.
	// Costs 20 bytes each against 4032.
	//
	//
	// Every effect gets a slot whether it is routed or not, so that
	// the routing below can index this list and an unrouted effect
	// still has somewhere to keep its values.  Except the settings,
	// which belong to the pedal - their entry is left zeroed, and a
	// zero id_hash matches nothing on the way back in.
	//
	scene->nr_effects = EFFECT_COUNT;
	for (int i = 0; i < EFFECT_COUNT; i++) {
		if (effects[i] == &settings_effect)
			continue;
		scene_save_effect(effects[i], &scene->effects[i]);
	}

	scene->nr_routed = routed_effect_count;
	for (int i = 0; i < routed_effect_count; i++)
		scene->routing[i] = effect_chain[i];

	//
	// The entry list above is written in effect id order, so an
	// index into it and an effect id are the same number here and
	// nothing has to be translated on the way out.  Only the load
	// side does work, because only the load side can find the list
	// numbered differently than it left it.
	//
	scene->nr_rules = nr_scene_rules;
	for (unsigned int i = 0; i < nr_scene_rules; i++)
		scene->rules[i] = scene_rules[i];

	return save_commit(SCENE_KEY(scene_id));
}

//
// The settings, which are the pedal's rather than the song's.
//
// They used to live in each scene's last slot, which meant changing
// scene could change the MIDI channel or the USB routing out from under
// whatever was listening.  That was an accident of there being sixteen
// slots and settings needing one, not a decision.
//
static bool load_globals(void)
{
	const struct globals_payload *globals;

	reset_effect(&settings_effect);

	nr_global_rules = 0;

	globals = save_read(GLOBALS_KEY);
	if (!globals || globals->version != SCENE_VERSION) {
		resolve_rules();
		return false;
	}

	if (settings_effect.id_hash == globals->settings.id_hash)
		scene_load_effect(&settings_effect, &globals->settings);

	uint8_t remap[SCENE_MAX_EFFECTS];

	memset(remap, 0xFF, sizeof(remap));
	for (int i = 0; i < SCENE_MAX_EFFECTS; i++) {
		for (int n = 0; n < EFFECT_COUNT; n++) {
			if (effects[n]->id_hash == globals->effect_ids[i]) {
				remap[i] = n;
				break;
			}
		}
	}

	scene_load_rules(global_rules, &nr_global_rules,
			 globals->rules, globals->nr_rules, remap);
	resolve_rules();

	return true;
}

static bool save_globals(void)
{
	struct globals_payload *globals;

	globals = (struct globals_payload *)save_buffer();
	globals->version = SCENE_VERSION;
	scene_save_effect(&settings_effect, &globals->settings);

	for (int i = 0; i < EFFECT_COUNT; i++)
		globals->effect_ids[i] = effects[i]->id_hash;

	globals->nr_rules = nr_global_rules;
	for (unsigned int i = 0; i < nr_global_rules; i++)
		globals->rules[i] = global_rules[i];

	return save_commit(GLOBALS_KEY);
}

//
// Which scenes have something in them.
//
// The app needs this to avoid offering a scene that has never been
// saved - see issue 69 - and it is nearly free, because a scene either
// has a valid slot under its key or it does not.
//
static uint32_t populated_scenes(void)
{
	uint32_t mask = 0;

	for (int i = 0; i < MAX_SCENES; i++) {
		if (save_read(SCENE_KEY(i)))
			mask |= 1u << i;
	}
	return mask;
}

#endif // SCENE_H
