#include "lfo.h"

sample_t get_usb_audio_input(void);

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
	unsigned int mix_pot;
	float def_mix;
	volatile unsigned int seq, last;
	unsigned char intense, active_pot;
	unsigned char pot_values[2][10];
	void (*init)(unsigned char[10]);
	void (*load)(struct effect *, unsigned char[10]);
	void (*save)(struct effect *, unsigned char[10]);
	float (*step)(float);
	const struct pot_descr pots[10];
};

#define EFFECT_POT(...) { __VA_ARGS__ }

// Effects and MIDI mapping auto-generated from scripts/gen_effects.py
extern uint8_t effect_chain[15];
extern uint8_t effect_chain_len;
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

// Effects are purely mono... For now
static inline sample_t do_effect_step(struct effect *effect, sample_t val)
{
	if (effect->mix != effect->target) {
		int dir = effect->mix < effect->target ? +1 : -1;
		effect->mix += dir;
	}

	if (effect->mix == 0) return val;

	float mix = effect->mix / (float) EFF_ENABLE_STEPS;
	float effect_val = effect->step(val.left);
	val.left = linear(mix, val.left, effect_val);
	val.right = linear(mix, val.right, effect_val);

	return val;
}

#include "process.h"

static int disable_all;

#define BLOCKSIZE 200

static raw_sample_t __attribute__((aligned(128))) i2s_dma_buf[16];
static int dma_tx;
static int dma_rx;
static unsigned int cpu_idx = 0;

static inline raw_sample_t *i2s_dma_tx_ptr(void)
{
	return (raw_sample_t *) (dma_hw->ch[dma_tx].read_addr & ~7);
}

static inline raw_sample_t *i2s_dma_rx_ptr(void)
{
	return (raw_sample_t *) (dma_hw->ch[dma_rx].write_addr & ~7);
}

static inline void single_sample(float mix)
{
	raw_sample_t *cpu_ptr = i2s_dma_buf + cpu_idx;
	cpu_idx = (cpu_idx + 1) & 15;

	// Wait for RX DMA to produce the sample
	while (cpu_ptr == i2s_dma_rx_ptr())
		tight_loop_contents();

	// Check we're safely ahead of TX DMA
	unsigned int tx_idx = i2s_dma_tx_ptr() - i2s_dma_buf;
	if (((cpu_idx - tx_idx) & 15) < 2) {
		dropped++;
		clipping = 1;
	}

	// In-place processing
	raw_sample_t sample = *cpu_ptr;
	sample_t in = process_input(sample);
	sample_t usb_in = get_usb_audio_input();

	// We need to do the USB input as stereo too
	if (settings.usb_input == USB_IN_PRE_FX) {
		in.left += usb_in.left;
		in.right += usb_in.right;
	}

	sample_t out = in;
	out = do_effect_step(effects[0], out); // Gate is always index 0 and runs first
	for (int i = 0; i < effect_chain_len; i++) {
		out = do_effect_step(effects[effect_chain[i]], out);
	}

	out.left = linear(mix, in.left, out.left);
	out.right = linear(mix, in.right, out.right);

	if (settings.usb_input == USB_IN_MIX) {
		out.left += usb_in.left;
		out.right += usb_in.right;
	}

	*cpu_ptr = process_output(out, sample);
}

static void bypass(void)
{
	for (int i = 0; i < BLOCKSIZE; i++) {
		single_sample(0.0);
	}
}

static __attribute__((noinline)) void make_one_noise(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];

		unsigned seq = effect->seq;
		if (seq == effect->last)
			continue;
		effect->last = seq;
		if (effect->mix)
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
