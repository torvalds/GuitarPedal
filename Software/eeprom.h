#ifndef EEPROM_H
#define EEPROM_H

#include "hardware/i2c.h"
#include "board.h"
#include <string.h>

struct effect_state {
	unsigned char pots[10];
	unsigned char enabled;
	unsigned char magic;
	unsigned char reserved[4]; // Pad to 16 bytes
};

// Cache the eeprom in RAM for easy access
//
// 2kbit eeprom: 256 bytes, 16 effects of 16 bytes each
#define MAX_SAVED_EFFECTS 16
static union {
	struct effect_state state[MAX_SAVED_EFFECTS];
	unsigned char bytes[256];
} eeprom_cache;
static uint32_t eeprom_dirty_mask = 0;

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
	uint8_t addr = 0;

	for (int try = 0; try < 10; try++) {
		if (i2c_write_blocking(MC24C02_I2C, &addr, 1, true) < 0) {
			sleep_ms(5);
			continue;
		}

		if (i2c_read_blocking(MC24C02_I2C, eeprom_cache.bytes, 256, false) == 256)
			return true;
	}
	memset(eeprom_cache.bytes, 0, 256);
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

	// Write at most one 8-byte chunk per call to handle the 5ms
	// latency. We use 8-byte chunks (rather than 16) for broader
	// compatibility, as standard 24C02 parts have an 8-byte page
	// write limit (unlike the 24C02C which supports 16 bytes).
	//
	// Isolate the lowest bit and clear it.
	mask &= -mask;
	eeprom_dirty_mask &= ~mask;

	unsigned int chunk_idx = ffs(mask) - 1;

	uint8_t buf[1 + 8];
	buf[0] = chunk_idx * 8;
	memcpy(buf + 1, eeprom_cache.bytes + chunk_idx * 8, 8);

	// If this fails, it fails..
	i2c_write_blocking(MC24C02_I2C, buf, sizeof(buf), false);
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
	if (effect_idx >= MAX_SAVED_EFFECTS)
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
	if (effect_idx >= MAX_SAVED_EFFECTS)
		return false;

	struct effect_state *state = eeprom_cache.state + effect_idx;
	int seq = effect->seq & 1;

	if (effect->save)
		effect->save(effect, effect->pot_values[seq]);
	memcpy(state->pots, effect->pot_values[seq], 10);

	state->enabled = effect->target ? 1 : 0;
	state->magic = effect_checksum(effect, state);

	// Set 2 bits per effect (each chunk is 8 bytes, effect is 16 bytes)
	eeprom_dirty_mask |= (3u << (effect_idx * 2));
	return true;
}

#endif
