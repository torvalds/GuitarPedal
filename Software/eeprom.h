#ifndef EEPROM_H
#define EEPROM_H

#include "hardware/i2c.h"
#include "board.h"
#include "status.h"
#include <string.h>

#if EEPROM_64KBIT
  #define MAX_SCENES 32
#else
  #define MAX_SCENES 1
#endif
#define MAX_SCENE_EFFECTS 16

// Some old 24c02 eeprom chips only do 8-byte page sizes,
// but the one I have is 16 bytes, and the MC24C64 has
// a 64-byte page size, but 16 byte writes work for both,
// and matches the effect size (so writing one chunk only
// changes one effect).
#define EEPROM_PAGE_SIZE 16
#define SCENE_SIZE (16*MAX_SCENE_EFFECTS)

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
extern volatile bool ui_sync_changed;

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
	const size_t size = sizeof(eeprom_cache.bytes);
#if EEPROM_64KBIT
	uint8_t addr[2] = { 0, 0 };
#else
	uint8_t addr[1] = { 0 };
#endif

	for (int try = 0; try < 10; try++) {
		if (i2c_write_blocking(MC24Cxx_I2C, addr, sizeof(addr), true) < 0) {
			sleep_ms(5);
			continue;
		}
		if (i2c_read_blocking(MC24Cxx_I2C, eeprom_cache.bytes, size, false) == size) {
			return true;
		}
	}
	memset(eeprom_cache.bytes, 0, size);
	return false;
}

// Called together with the UI update, at 25Hz
//
// That makes it safe to write to the eeprom, which has
// a write latency of up to 5ms
static void eeprom_task(void)
{
	for (int scene = 0; scene < MAX_SCENES; scene++) {
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

		uint8_t *p = buf;
		size_t len = sizeof(buf);
#if !EEPROM_64KBIT
		p++; len--;
#endif
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

static int find_effect_slot(struct effect *effect)
{
	if (effect == effects[EFFECT_COUNT - 1]) return 15;
	if (effect == effects[0]) return 0;
	for (int i = 0; i < routed_effect_count; i++) {
		if (effects[effect_chain[i]] == effect) return i + 1;
	}
	return -1;
}

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
	effect->mix_pot = (state->mix_level * EFF_ENABLE_STEPS) / 127;
	effect->target = (slot == 0 || slot == 15 || slot <= routed_effect_count) ? effect->mix_pot : 0;
	effect->mix = effect->target;
	if (effect->init)
		effect->init(effect->pot_values[0]);
	if (effect->load)
		effect->load(effect, state->pots);
	return true;
}

static bool save_effect_state(unsigned int effect_idx, struct effect *effect)
{
	int slot = find_effect_slot(effect);
	if (slot < 0) return false;

	struct effect_state *state = &eeprom_cache.state[current_scene_id][slot];
	int seq = effect->seq & 1;

	if (effect->save)
		effect->save(effect, effect->pot_values[seq]);
	memcpy(state->pots, effect->pot_values[seq], 10);

	state->mix_level = (effect->mix_pot * 127) / EFF_ENABLE_STEPS;
	// Note: we leave reserved[0] (next_id) unchanged to avoid messing with routing
	state->magic = effect_checksum(effect, state);

	eeprom_dirty_mask[current_scene_id] |= 1u << slot;
	return true;
}


static bool load_scene(uint8_t scene_id)
{
	if (scene_id >= MAX_SCENES) return false;

	current_scene_id = scene_id;
	routed_effect_count = 0;
	extern struct effect gate_effect;
	extern struct effect settings_effect;

	if (!load_effect_state_from_slot(0, effects[0])) {
		// If corrupt, fallback
	}

	uint8_t next_id = eeprom_cache.state[current_scene_id][0].reserved[0];
	int current_slot = 1;

	while (next_id != 0xFF && routed_effect_count < 14 && current_slot < 15) {
		if (next_id > 0 && next_id < EFFECT_COUNT - 1) {
			effect_chain[routed_effect_count++] = next_id;
			load_effect_state_from_slot(current_slot, effects[next_id]);
		}
		next_id = eeprom_cache.state[current_scene_id][current_slot].reserved[0];
		current_slot++;
	}

	// Always load settings from slot 15
	load_effect_state_from_slot(15, &settings_effect);

	extern uint8_t effect_chain_len;
	effect_chain_len = routed_effect_count;
	for (int i = 1; i < EFFECT_COUNT - 1; i++) {
		bool routed = false;
		for (int j = 0; j < routed_effect_count; j++) {
			if (effect_chain[j] == i) { routed = true; break; }
		}
		if (!routed) {
			effect_chain[effect_chain_len++] = i;
			effects[i]->target = 0;
		}
	}
	return true;
}

static bool save_scene(uint8_t scene_id)
{
	if (scene_id >= MAX_SCENES) return false;

	current_scene_id = scene_id;

	struct effect *gate_eff = effects[0];
	struct effect_state *gate_state = &eeprom_cache.state[current_scene_id][0];
	int gate_seq = gate_eff->seq & 1;
	if (gate_eff->save)
		gate_eff->save(gate_eff, gate_eff->pot_values[gate_seq]);
	memcpy(gate_state->pots, gate_eff->pot_values[gate_seq], 10);

	gate_state->mix_level = (gate_eff->mix_pot * 127) / EFF_ENABLE_STEPS;
	gate_state->reserved[0] = (0 < routed_effect_count) ? effect_chain[0] : 0xFF;
	gate_state->magic = effect_checksum(gate_eff, gate_state);

	for (int i = 0; i < routed_effect_count; i++) {
		struct effect *e = effects[effect_chain[i]];
		struct effect_state *state = &eeprom_cache.state[current_scene_id][i + 1];
		int seq = e->seq & 1;

		if (e->save) e->save(e, e->pot_values[seq]);
		memcpy(state->pots, e->pot_values[seq], 10);
		state->mix_level = (e->mix_pot * 127) / EFF_ENABLE_STEPS;
		state->reserved[0] = (i + 1 < routed_effect_count) ? effect_chain[i+1] : 0xFF;
		state->magic = effect_checksum(e, state);
	}

	extern struct effect settings_effect;
	struct effect_state *state15 = &eeprom_cache.state[current_scene_id][15];
	int seq15 = settings_effect.seq & 1;
	if (settings_effect.save)
		settings_effect.save(&settings_effect, settings_effect.pot_values[seq15]);
	memcpy(state15->pots, settings_effect.pot_values[seq15], 10);

	state15->mix_level = (settings_effect.mix_pot * 127) / EFF_ENABLE_STEPS;
	state15->reserved[0] = 0xFF;
	state15->magic = effect_checksum(&settings_effect, state15);

	eeprom_dirty_mask[current_scene_id] = (1 << MAX_SCENE_EFFECTS) - 1; // Mark all 16 slots as dirty
	return true;
}

#endif
