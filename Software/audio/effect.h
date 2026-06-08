#include "lfo.h"

float get_usb_audio_input(void);

typedef float (*pot_convert_fn)(unsigned char);

struct pot_descr {
	const char *label;
	const char *unit;
	pot_convert_fn convert;
	unsigned char def_val;
	const char *const *enum_names;
};

//
// Primary interface for audio DSP algorithms.
//
// Each effect plugin defines its runtime processing callbacks and UI
// layout.
//
// Note: effects[] may leave `pots` array with NULL labels, which tells
// the OLED and I2C pollers to omit querying/drawing parameters that the
// hook doesn't use.
//
// We have two set of pot values, because cpu 0 - the UI core - will
// prepare the pot state in the inactive set, and then atomically switch
// that state so that code 1 - the audio core - will never use pot
// values that are in some halfway state. The active state is the LSB of
// the 'seq' value, which is used to tell whether the state has changed.
//
// 'mix' is the current mixing state, and 'target' is the target mixing
// state for fading in and fading out the effect (in fractions of
// EFF_ENABLE_STEPS)
//
struct effect {
	const char *name, *short_name;
	unsigned int mix, target;
	volatile unsigned int seq, last;
	unsigned char intense, active_pot;
	unsigned char pot_values[2][10];
	void (*graph)(struct effect *, int, unsigned char[10]);
	void (*init)(unsigned char[10]);
	void (*load)(struct effect *, unsigned char[10]);
	void (*save)(struct effect *, unsigned char[10]);
	float (*step)(float);
	const struct pot_descr pots[10];
};

#define EFFECT_POT(...) { __VA_ARGS__ }

// Effects and MIDI mapping auto-generated from scripts/gen_effects.py
#include "effect_map.h"

static inline void generic_effect_describe(struct effect *e, unsigned char pots[10])
{
	for (int i = 0; i < 10; i++) {
		if (e->pots[i].label) {
			const char *unit = e->pots[i].unit ? e->pots[i].unit : "";
			if (e->pots[i].enum_names) {
				int idx = (int)e->pots[i].convert(pots[i]);
				fprintf(stderr, " %s=%s %s", e->pots[i].label, e->pots[i].enum_names[idx], unit);
			} else if (e->pots[i].convert) {
				float val = e->pots[i].convert(pots[i]);
				fprintf(stderr, " %s=%g %s", e->pots[i].label, val, unit);
			}
		}
	}
	fprintf(stderr, "\n");
}

static unsigned int dropped;

static inline float do_effect_step(struct effect *effect, float val)
{
	if (effect->mix == effect->target) {
		if (effect->mix)
			val = effect->step(val);
	} else {
		int dir = effect->mix < effect->target ? +1 : -1;
		float mix = effect->mix / (float) EFF_ENABLE_STEPS;
		effect->mix += dir;
		float effect_val = effect->step(val);
		val = linear(mix, val, effect_val);
	}
	return val;
}

#include "process.h"

static int disable_all;

#define BLOCKSIZE 200
static void bypass(void)
{
	PIO pio = pio0;

	for (int i = 0; i < BLOCKSIZE; i++) {
		int32_t sample = pio_sm_get_blocking(pio, PIO0_I2S_RX_SM) << 8;
		float val = process_input(sample);
		analyze_process_sample(val);
		sample = process_output(val, sample);
		pio_sm_put_blocking(pio0, PIO0_I2S_TX_SM, sample);
	}
}

static inline void single_sample(float mix)
{
	PIO pio = pio0;

	int32_t sample = pio_sm_get_blocking(pio, PIO0_I2S_RX_SM) << 8;
	float in = process_input(sample);
	float usb_in = get_usb_audio_input();

	if (settings.usb_input == USB_IN_PRE_FX)
		in += usb_in;

	float out = in;
	for (int i = 0; i < ARRAY_SIZE(effects); i++)
		out = do_effect_step(effects[i], out);

	float val = linear(mix, in, out);

	if (settings.usb_input == USB_IN_MIX)
		val += usb_in;

	sample = process_output(val, sample);

	if (pio_sm_is_tx_fifo_empty(pio, PIO0_I2S_TX_SM)) {
		dropped++;
		clipping = 1;
	}
	pio_sm_put_blocking(pio0, PIO0_I2S_TX_SM, sample);
}

static __attribute__((noinline)) void make_one_noise(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];

		unsigned seq = effect->seq;
		if (seq == effect->last)
			continue;
		effect->last = seq;
		effect->init(effect->pot_values[seq & 1]);
	}

	static int disable = 0;
	while (disable != disable_all) {
		float mix = disable / (float) EFF_ENABLE_STEPS;
		disable += (disable < disable_all) ? 1 : -1;
		single_sample(mix);
	}

	if (disable)
		return bypass();

	for (int i = 0; i < BLOCKSIZE; i++)
		single_sample(1.0);
}
