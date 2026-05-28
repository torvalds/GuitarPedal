#ifndef EEPROM_H
#define EEPROM_H

#include "hardware/i2c.h"
#include "board.h"
#include <string.h>

struct effect_state {
	signed char pots[10];
	unsigned char enabled;
	unsigned char magic;
	unsigned char reserved[4]; // Pad to 16 bytes
};

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

static inline bool load_effect_state(int effect_idx, struct effect *effect)
{
	struct effect_state state;
	uint8_t addr = effect_idx * 16; // 1 page (16 bytes) per effect

	// The EEPROM may not be ready immediately at boot. Retry for up to ~50ms.
	int retries = 10;
	while (retries-- > 0) {
		// Write the memory address we want to read from (with nostop = true)
		if (i2c_write_blocking(MC24C02_I2C, &addr, 1, true) >= 0) {
			break;
		}
		sleep_ms(5);
	}

	if (retries < 0) {
		return false;
	}

	// Read 16 bytes sequentially
	int read_res = i2c_read_blocking(MC24C02_I2C, (uint8_t *)&state, sizeof(state), false);
	if (read_res == sizeof(state)) {
		if (state.magic == effect_checksum(effect, &state)) {
			int seq = effect->seq & 1;
			memcpy(effect->pot_values[seq], state.pots, 10);
			effect->target = state.enabled ? EFF_ENABLE_STEPS : 0;
			effect->mix = effect->target; // Apply immediately on load
			if (effect->load)
				effect->load(effect, state.pots);
			return true;
		}
	}
	return false;
}

static inline void save_effect_state(int effect_idx, struct effect *effect)
{
	struct effect_state state = { };
	int seq = effect->seq & 1;

	if (effect->save)
		effect->save(effect, effect->pot_values[seq]);
	memcpy(state.pots, effect->pot_values[seq], 10);

	state.enabled = effect->target ? 1 : 0;
	state.magic = effect_checksum(effect, &state);

	// Prepare the I2C transaction: [Address byte] + [16 bytes data]
	uint8_t buf[1 + sizeof(state)];
	buf[0] = effect_idx * 16; // Page aligned address
	memcpy(buf + 1, &state, sizeof(state));

	i2c_write_blocking(MC24C02_I2C, buf, sizeof(buf), false);
}

#endif
