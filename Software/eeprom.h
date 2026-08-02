#ifndef EEPROM_H
#define EEPROM_H

#include "hardware/i2c.h"
#include "board.h"
#include "status.h"
#include <string.h>

//
// Room for the largest eeprom we support.  How many of these scenes
// actually exist is decided at runtime by eeprom_set_geometry(), because
// which part is fitted is a property of the board and not of the build.
//
#define MAX_SCENES 32
#define MAX_SCENE_EFFECTS 16

// Some old 24c02 eeprom chips only do 8-byte page sizes,
// but the one I have is 16 bytes, and the MC24C64 has
// a 64-byte page size, but 16 byte writes work for both,
// and matches the effect size (so writing one chunk only
// changes one effect).
#define EEPROM_PAGE_SIZE 16
#define SCENE_SIZE (16*MAX_SCENE_EFFECTS)

// Slot 0 is the noise gate, the last slot is the settings, and the
// routed chain has to fit in between.
_Static_assert(MAX_ROUTED_EFFECTS + 2 <= MAX_SCENE_EFFECTS,
	       "a scene has no room for that many routed effects");
#define SETTINGS_SLOT (MAX_SCENE_EFFECTS - 1)

//
// The mix is stored on the same 0..120 scale as every other pot, so
// POT_TO_FLOAT()/FLOAT_TO_POT() convert it like any other.  It used to
// be 0..127, which meant a save/load round trip quietly moved it by a
// step - the two scalings don't divide into each other.
//
struct effect_state {
	unsigned char pots[10];
	unsigned char mix_level;
	unsigned char magic;
	unsigned char reserved[4]; // Pad to 16 bytes
};

// Cache the entire EEPROM in RAM for easy access
static union {
	struct effect_state state[MAX_SCENES][MAX_SCENE_EFFECTS];
	unsigned char bytes[MAX_SCENES * SCENE_SIZE];
} eeprom_cache;
static uint16_t eeprom_dirty_mask[MAX_SCENES] = {0};
static uint8_t current_scene_id = 0;

//
// Which part turned out to be fitted.
//
// The 64kbit one takes a two-byte address and holds all 32 scenes; the
// 2kbit one on the early boards takes one byte and holds a single scene.
// Starts as the larger because that is what the read in
// eeprom_set_geometry() has to assume in order to ask the question.
//
static unsigned int eeprom_addr_bytes = 2;
static unsigned int nr_scenes = MAX_SCENES;

static inline uint8_t string_checksum(const char *cstr)
{
	uint8_t sum = 0;

	if (cstr) {
		while (*cstr)
			sum += *cstr++;
	}
	return sum;
}

static inline uint8_t effect_checksum(struct effect *effect, struct effect_state *state)
{
	uint8_t sum = 0;

	sum += string_checksum(effect->name);

	for (int i = 0; i < 10; i++) {
		const struct pot_descr *descr = effect->pots + i;
		sum += string_checksum(descr->label);
		const char *const *enums = descr->enum_names;
		if (enums) {
			while (*enums)
				sum += string_checksum(*enums++);
		}
		sum += (uint8_t)effect->pots[i].def_val;
		sum += (uint8_t)state->pots[i];
	}

	sum += (uint8_t)state->mix_level;
	return sum;
}

// We can read the whole eeprom in one go, but we may
// need to wait for it to wake up.
static bool init_eeprom(void)
{
	const size_t size = nr_scenes * SCENE_SIZE;
	uint8_t addr[2] = { 0, 0 };
	const uint8_t *ap = addr + (2 - eeprom_addr_bytes);

	for (int try = 0; try < 10; try++) {
		if (i2c_write_blocking(MC24Cxx_I2C, ap, eeprom_addr_bytes, true) < 0) {
			sleep_ms(5);
			continue;
		}
		if (i2c_read_blocking(MC24Cxx_I2C, eeprom_cache.bytes, size, false) == size) {
			return true;
		}
	}
	memset(eeprom_cache.bytes, 0, sizeof(eeprom_cache.bytes));
	return false;
}

//
// Does this addressing round-trip a page?
//
// Only reached on a blank part, so writing is free.  The pattern has to
// have no short period: addressing a 24C02 with two bytes feeds the low
// half into its page buffer as data, so what comes back is the pattern
// shifted, and a pattern that repeated would compare equal to its own
// shift and call a 2kbit part a 64kbit one.
//
static bool eeprom_probe_addressing(unsigned int bytes)
{
	uint8_t buf[2 + EEPROM_PAGE_SIZE];
	uint8_t back[EEPROM_PAGE_SIZE];
	uint8_t *addr = buf + (2 - bytes);
	int len = bytes + EEPROM_PAGE_SIZE;

	buf[0] = buf[1] = 0;
	for (unsigned int i = 0; i < EEPROM_PAGE_SIZE; i++)
		buf[2 + i] = 0x5a + i * 7;

	if (i2c_write_blocking(MC24Cxx_I2C, addr, len, false) != len)
		return false;

	sleep_ms(10);			// the write cycle takes up to 5ms

	if (i2c_write_blocking(MC24Cxx_I2C, addr, bytes, true) < 0)
		return false;
	if (i2c_read_blocking(MC24Cxx_I2C, back, sizeof(back), false) != sizeof(back))
		return false;

	return !memcmp(back, buf + 2, sizeof(back));
}

//
// Say what was decided, and on what evidence.
//
// There are four ways out of eeprom_set_geometry() and three of them
// used to be silent, all three landing on the same answer - the 64kbit
// default - for completely different reasons.  From outside, a board
// that had been identified correctly and one that had given up looked
// identical, which is a bad property in the one decision that everything
// else about storage depends on.
//
// This is *recorded*, not reported.  The status mailbox is one slot with
// overwrite semantics and the first reader takes the message away, which
// makes it the wrong shape for this twice over: probe_hardware() runs
// one line later and overwrites whatever is in there, and even without
// that, whether anybody ever saw a startup message would depend on
// whether they happened to be connected for the first read after boot.
//
// Which part is fitted is not an event, it is what this board *is*, so
// it goes out with the identity where it can be asked for and answers
// the same every time.
//
// One buffer, because only one of these is ever produced: the decision
// is made once, at startup, before anything can read it.
//
// Formatted by hand: snprintf() brings newlib's float conversion with
// it, which is exactly what scripts/check-float.py is there to refuse.
static char eeprom_verdict[96];
static char *eeprom_vp = eeprom_verdict;

static void ep_str(const char *s)
{
	char *end = eeprom_verdict + sizeof(eeprom_verdict) - 1;

	while (*s && eeprom_vp < end)
		*eeprom_vp++ = *s++;
	*eeprom_vp = 0;
}

static void ep_num(unsigned long v, unsigned int base)
{
	char digits[16];
	int n = 0;

	do {
		digits[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);

	while (n) {
		char one[2] = { digits[--n], 0 };
		ep_str(one);
	}
}

//
// Which of the two eeproms is fitted, and how to talk to it.
//
// The obvious test does not work, and it is worth saying why before
// somebody reinvents it.  A 2kbit part read as though it were the
// 64kbit one *ought* to answer by wrapping, its address counter rolling
// over every 256 bytes, so that 8K comes back 256-periodic.  That is
// true of most of the family and it is not true of the part on these
// boards.  From the M24C02 datasheet:
//
//	For device delivered in DFN5 package, after the last memory
//	address (FFh for a 2Kbit), the address counter doesn't roll-over
//	to the memory address 00h.  The next addresses and data bytes
//	outputted are therefore undefined and not guaranteed.
//
// DFN5 is the tiny package, chosen for being small and simple and for
// needing no address strapping at all: it answers at 0x50 and that is
// the end of it.  The TAC5112 was then strapped around *it* - the codec
// has one address pin selecting among 0x50..0x53, and a 4.7k to ground
// puts it at 0x51.  So the package whose footnote causes all this was
// picked for reasons that had nothing to do with any of it.
//
// The read past 255 therefore returns undefined data, the periodicity
// test sees noise, and the part gets called a 64kbit one - which is what
// happened on every early board, silently, until somebody tried to save
// a scene.
//
// Note the asymmetry that is left: wrapping still *proves* the small
// part, because nothing else would produce it.  Not wrapping proves
// nothing at all.  So the test is kept, one-sided, below the thing that
// actually decides.
//
// What decides is the codec.  Every board with a TAC5112 on it was
// built with the 2kbit part, and that is a fact about how these boards
// were made rather than about how either chip behaves - so it is
// checked first and believed.  It is out-of-band knowledge and it is
// the only non-destructive signal available, the alternative being to
// write to a part whose geometry is the thing in question.
//
// The real answer is a header that says what the part is instead of a
// probe that guesses - see the storage rework, where the format grows
// one anyway.
//
// Examining the whole array rather than a window matters.  An unused
// scene reads uniform, and a uniform *window* would look blank on a part
// that has data elsewhere - which would then have been written to by the
// probe below.  Uniform across all 8K is genuinely blank.
//
static void eeprom_set_geometry(bool early_board)
{
	const unsigned char *b = eeprom_cache.bytes;
	bool varies = false, repeats = true;
	size_t first_diff = 0, breaks = 0;

	if (early_board) {
		eeprom_addr_bytes = 1;
		nr_scenes = 1;
		if (!init_eeprom()) {
			ep_str("2kbit by the codec, but it did not answer");
			return;
		}
		ep_str("2kbit: a TAC5112 answered, and those boards all have it");
		return;
	}

	if (!init_eeprom()) {
		ep_str("no answer to the initial read, assuming 64kbit");
		return;
	}

	for (size_t i = 0; i < sizeof(eeprom_cache.bytes); i++) {
		if (b[i] != b[0])
			varies = true;
		if (i >= 256 && b[i] != b[i - 256]) {
			if (repeats)
				first_diff = i;
			repeats = false;
			breaks++;
		}
	}

	if (varies) {
		if (repeats) {
			// Wrapped, so it is the small part
			eeprom_addr_bytes = 1;
			nr_scenes = 1;
			init_eeprom();
			ep_str("2kbit: the read wrapped at 256");
			return;
		}
		//
		// Has content and does not wrap, so it is taken to be
		// the big part.  Worth saying which byte settled it: if
		// this is wrong, that offset is where to look.
		//
		//
		// Not wrapping is not evidence - see above, the DFN5 part
		// does not wrap either.  This is the 64kbit answer by
		// default rather than by demonstration, so the numbers are
		// here to be looked at rather than trusted.
		//
		ep_str("64kbit: not 256-periodic, first at ");
		ep_num(first_diff, 10);
		ep_str(" (");
		ep_num(b[first_diff], 16);
		ep_str(" vs ");
		ep_num(b[first_diff - 256], 16);
		ep_str("), ");
		ep_num(breaks, 10);
		ep_str(" breaks in ");
		ep_num(sizeof(eeprom_cache.bytes) - 256, 10);
		return;
	}

	//
	// Every byte the same, so it is blank and there is nothing to lose
	// by writing to it - which is the only way left to tell.  Write a
	// page and read it back with each addressing in turn and see which
	// round-trips.  Insisting on a positive result for whichever is
	// chosen is much stronger than inferring from a failure.
	//
	// Afterwards the array has content, so this is the only boot that
	// ever needs it: the read above settles it from then on.
	//
	if (eeprom_probe_addressing(2))
		ep_str("64kbit: blank, two-byte addressing round-tripped");
	else if (eeprom_probe_addressing(1)) {
		eeprom_addr_bytes = 1;
		nr_scenes = 1;
		ep_str("2kbit: blank, one-byte addressing round-tripped");
	} else {
		ep_str("blank, neither addressing round-tripped, assuming 64kbit");
	}
	init_eeprom();
}

// Called together with the UI update, at 25Hz
//
// That makes it safe to write to the eeprom, which has
// a write latency of up to 5ms
static void eeprom_task(void)
{
	for (unsigned int scene = 0; scene < nr_scenes; scene++) {
		uint16_t mask = eeprom_dirty_mask[scene];
		if (!mask) continue;

		// Write at most a page per call to handle the 5ms
		// latency.
		//
		// Isolate the lowest bit.
		mask &= -mask;
		eeprom_dirty_mask[scene] &= ~mask;

		unsigned int chunk_idx = ffs(mask) - 1;
		unsigned int base_offset = scene * SCENE_SIZE;
		unsigned int offset = base_offset + chunk_idx * EEPROM_PAGE_SIZE;

		uint8_t buf[2 + EEPROM_PAGE_SIZE];
		buf[0] = offset >> 8;
		buf[1] = offset & 0xff;
		memcpy(buf + 2, &eeprom_cache.state[scene][chunk_idx], EEPROM_PAGE_SIZE);

		uint8_t *p = buf + (2 - eeprom_addr_bytes);
		size_t len = EEPROM_PAGE_SIZE + eeprom_addr_bytes;
		if (i2c_write_blocking(MC24Cxx_I2C, p, len, false) != len)
			report_status("EEPROM write failed");
		return;
	}
}

static int max_pot_val(struct effect *effect, int pot)
{
	const struct pot_descr *desc = effect->pots + pot;
	if (!desc->label)
		return 0;

	const char *const *enums = desc->enum_names;
	if (!enums)
		return 120;

	// Valid values for enumeration pots are 0..N-1
	//
	// An empty enumeration pot isn't valid and can
	// never be loaded from eeprom
	for (int i = 0; ; i++) {
		if (!enums[i])
			return i-1;
	}
}

extern uint8_t routed_effect_count;


static bool load_effect_state_from_slot(unsigned int slot, struct effect *effect)
{
	if (slot >= MAX_SCENE_EFFECTS)
		return false;

	struct effect_state *state = &eeprom_cache.state[current_scene_id][slot];

	if (state->magic != effect_checksum(effect, state))
		return false;

	for (int i = 0; i < 10; i++) {
		int max_val = max_pot_val(effect, i);
		if (state->pots[i] > max_val)
			return false;
	}

	memcpy(effect->pot_values[0], state->pots, 10);
	memcpy(effect->pot_values[1], state->pots, 10);
	set_mix_pot(effect, POT_TO_FLOAT(state->mix_level));
	effect->target = EFF_ENABLE_STEPS;
	effect->mix = effect->target;
	if (effect->init)
		effect->init(effect->pot_values[0]);
	if (effect->load)
		effect->load(effect, state->pots);
	return true;
}



static bool load_scene(uint8_t scene_id)
{
	if (scene_id >= nr_scenes) return false;

	current_scene_id = scene_id;

	load_effect_state_from_slot(0, effects[0]);

	// The chain is a linked list through each slot's 'reserved[0]'.
	// routing_add() rejects anything bogus, so a corrupt scene can't
	// build a chain with repeats in it or run off the end.
	routing_bitmap_t routable = routing_start();

	uint8_t next_id = eeprom_cache.state[current_scene_id][0].reserved[0];
	int current_slot = 1;

	while (next_id != 0xFF && current_slot < SETTINGS_SLOT) {
		if (routing_add(&routable, next_id))
			load_effect_state_from_slot(current_slot, effects[next_id]);
		next_id = eeprom_cache.state[current_scene_id][current_slot].reserved[0];
		current_slot++;
	}

	load_effect_state_from_slot(SETTINGS_SLOT, &settings_effect);

	routing_end(routable);
	return true;
}

static bool save_scene(uint8_t scene_id)
{
	if (scene_id >= nr_scenes) return false;

	current_scene_id = scene_id;

	struct effect *gate_eff = effects[0];
	struct effect_state *gate_state = &eeprom_cache.state[current_scene_id][0];
	int gate_seq = gate_eff->seq & 1;
	if (gate_eff->save)
		gate_eff->save(gate_eff, gate_eff->pot_values[gate_seq]);
	memcpy(gate_state->pots, gate_eff->pot_values[gate_seq], 10);

	gate_state->mix_level = FLOAT_TO_POT(gate_eff->mix_pot);
	gate_state->reserved[0] = (0 < routed_effect_count) ? effect_chain[0] : 0xFF;
	gate_state->magic = effect_checksum(gate_eff, gate_state);

	for (int i = 0; i < routed_effect_count; i++) {
		struct effect *e = effects[effect_chain[i]];
		struct effect_state *state = &eeprom_cache.state[current_scene_id][i + 1];
		int seq = e->seq & 1;

		if (e->save) e->save(e, e->pot_values[seq]);
		memcpy(state->pots, e->pot_values[seq], 10);
		state->mix_level = FLOAT_TO_POT(e->mix_pot);
		state->reserved[0] = (i + 1 < routed_effect_count) ? effect_chain[i+1] : 0xFF;
		state->magic = effect_checksum(e, state);
	}

	struct effect_state *state15 = &eeprom_cache.state[current_scene_id][SETTINGS_SLOT];
	int seq15 = settings_effect.seq & 1;
	if (settings_effect.save)
		settings_effect.save(&settings_effect, settings_effect.pot_values[seq15]);
	memcpy(state15->pots, settings_effect.pot_values[seq15], 10);

	state15->mix_level = FLOAT_TO_POT(settings_effect.mix_pot);
	state15->reserved[0] = 0xFF;
	state15->magic = effect_checksum(&settings_effect, state15);

	eeprom_dirty_mask[current_scene_id] = (1 << MAX_SCENE_EFFECTS) - 1; // Mark all 16 slots as dirty
	return true;
}

#endif
