#ifndef EEPROM_H
#define EEPROM_H

#include "hardware/i2c.h"
#include "board.h"
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
	unsigned char enabled;
	unsigned char magic;
	unsigned char reserved[4]; // Pad to 16 bytes
};

// Cache the current scene in RAM for easy access
static union {
	struct effect_state state[MAX_SCENE_EFFECTS];
	unsigned char bytes[SCENE_SIZE];
} eeprom_cache;
static uint32_t eeprom_dirty_mask = 0;
static uint8_t current_scene_id = 0;

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

	sum += (uint8_t)state->enabled;
	return sum;
}

// We can read the whole eeprom in one go, but we may
// need to wait for it to wake up.
static bool init_eeprom(void)
{
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

		if (i2c_read_blocking(MC24Cxx_I2C, eeprom_cache.bytes, SCENE_SIZE, false) == SCENE_SIZE)
			return true;
	}
	memset(eeprom_cache.bytes, 0, SCENE_SIZE);
	return false;
}

// Called together with the UI update, at 25Hz
//
// That makes it safe to write to the eeprom, which has
// a write latency of up to 5ms
static void eeprom_task(void)
{
	uint32_t mask = eeprom_dirty_mask;

	if (!mask)
		return;

	// Write at most a page per call to handle the 5ms
	// latency.
	//
	// Isolate the lowest bit and clear it.
	mask &= -mask;
	eeprom_dirty_mask &= ~mask;

	unsigned int chunk_idx = ffs(mask) - 1;
	unsigned int base_offset = current_scene_id * SCENE_SIZE;
	unsigned int offset = base_offset + chunk_idx * EEPROM_PAGE_SIZE;

	uint8_t buf[2 + EEPROM_PAGE_SIZE];
	buf[0] = offset >> 8;
	buf[1] = offset & 0xff;
	memcpy(buf + 2, eeprom_cache.bytes + offset, EEPROM_PAGE_SIZE);

	// If this fails, it fails..
	uint8_t *p = buf;
	size_t len = sizeof(buf);
#if !EEPROM_64KBIT
	p++; len--;
#endif
	i2c_write_blocking(MC24Cxx_I2C, p, len, false);
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

static bool load_effect_state(unsigned int effect_idx, struct effect *effect)
{
	if (effect_idx >= MAX_SCENE_EFFECTS)
		return false;

	struct effect_state *state = eeprom_cache.state + effect_idx;

	if (state->magic != effect_checksum(effect, state))
		return false;

	for (int i = 0; i < 10; i++) {
		int max_val = max_pot_val(effect, i);
		if (state->pots[i] > max_val)
			return false;
	}

	memcpy(effect->pot_values[0], state->pots, 10);
	memcpy(effect->pot_values[1], state->pots, 10);
	effect->target = state->enabled ? EFF_ENABLE_STEPS : 0;
	effect->mix = effect->target;
	if (effect->load)
		effect->load(effect, state->pots);
	return true;
}

static bool save_effect_state(unsigned int effect_idx, struct effect *effect)
{
	if (effect_idx >= MAX_SCENE_EFFECTS)
		return false;

	struct effect_state *state = eeprom_cache.state + effect_idx;
	int seq = effect->seq & 1;

	if (effect->save)
		effect->save(effect, effect->pot_values[seq]);
	memcpy(state->pots, effect->pot_values[seq], 10);

	state->enabled = effect->target ? 1 : 0;
	state->magic = effect_checksum(effect, state);

	eeprom_dirty_mask |= 1u << effect_idx;
	return true;
}

static bool load_scene(uint8_t scene_id)
{
	if (scene_id >= MAX_SCENES) return false;

	// Wait for any pending writes
	while (eeprom_dirty_mask) eeprom_task();

	uint16_t offset = scene_id * SCENE_SIZE;
#if EEPROM_64KBIT
	uint8_t addr[2] = { offset >> 8, offset & 0xff };
#else
	uint8_t addr[1] = { offset & 0xff };
#endif

	for (int try = 0; try < 10; try++) {
		if (i2c_write_blocking(MC24Cxx_I2C, addr, sizeof(addr), true) < 0) {
			sleep_ms(5);
			continue;
		}
		if (i2c_read_blocking(MC24Cxx_I2C, eeprom_cache.bytes, SCENE_SIZE, false) == SCENE_SIZE) {
			current_scene_id = scene_id;
			return true;
		}
	}
	return false;
}

static bool save_scene(uint8_t scene_id)
{
	if (scene_id >= MAX_SCENES) return false;

	// Ensure current state is fully written before switching pointer
	while (eeprom_dirty_mask) eeprom_task();

	// If we are saving to a DIFFERENT scene, we change current_scene_id
	// and mark everything as dirty so eeprom_task writes it out over time.
	// This avoids blocking the audio thread for 256 bytes of writes
	// (which takes ~80ms due to page write delays).
	current_scene_id = scene_id;
	eeprom_dirty_mask = (1 << EFFECT_COUNT) - 1; // Mark all used effects as dirty
	return true;
}

#endif
