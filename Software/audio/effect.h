#include "lfo.h"

//
// How long an effect takes to fade in or out, in samples - a tenth of a
// second.
//
// Enabling or disabling an effect walks 'mix' up to or down from this
// instead of switching, so that nothing clicks, and it is the unit
// 'target' is counted in.  Used from here, eeprom.h, ui.h and blink.c;
// it lived in effects/parametric_eq.h for a while, which worked only
// because priority 120 happened to be included ahead of everything that
// needed it.
//
#define EFF_ENABLE_STEPS ((int)SAMPLES_PER_SEC/10)

sample_t get_usb_audio_input(void);

typedef float (*pot_convert_fn)(unsigned char);

//
// How an effect's wet and dry get mixed together.
//
// Linear is right when the wet signal is a filtered version of the dry
// and the two stay correlated - amplitudes add, so half of each gives
// you back what you started with.  Almost everything here is like that.
//
// Equal power is right when they aren't correlated: a modulated delay,
// an echo tail, a reverb.  There the *powers* add rather than the
// amplitudes, and a linear mix leaves a 3dB hole in the middle of the
// sweep.  sin^2 + cos^2 == 1 fills it.
//
// Getting this backwards costs 3dB either way, so it is per effect
// rather than one law for all of them.
//
enum mix_law {
	MIX_LINEAR,
	MIX_POWER,
};

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
	float mix_pot;
	float def_mix;
	enum mix_law mix_law;

	//
	// Set by 'MIX: NONE': this is not in the wet-and-dry business
	// at all, and has no 'step' either - whoever runs it calls it
	// by name.  So the chain's idea of how much of it you are
	// hearing does not describe it, and neither does its idea of
	// whether it is running - see make_one_noise().
	//
	unsigned char no_mix;

	// What the mix law works out to, and where we are on the way
	// there. Slewed rather than applied straight so that dragging
	// the mix around doesn't click.
	float dry, wet;
	float dry_target, wet_target;
	unsigned int seq, last;
	unsigned char intense, active_pot;
	unsigned char pot_values[2][10];
	void (*init)(unsigned char[10]);
	void (*load)(struct effect *, unsigned char[10]);
	void (*save)(struct effect *, unsigned char[10]);
	sample_t (*step)(sample_t, float dry, float wet);
	const struct pot_descr pots[10];
};

#define EFFECT_POT(...) { __VA_ARGS__ }

//
// How many effects one scene can route.
//
// This is a storage limit, not a limit on how many effects exist - the
// point of routing is that there are more effects to choose from than
// you can have in a chain at once.  eeprom.h checks that a scene has the
// slots for it.
//
#define MAX_ROUTED_EFFECTS 14

// Effects and MIDI mapping auto-generated from scripts/gen_effects.py
extern uint8_t effect_chain[MAX_ROUTED_EFFECTS];
extern uint8_t routed_effect_count;
#include "effect_map.h"


//
// Work out the two multipliers for a mix setting.
//
// Only called when the setting changes, which is why the equal-power
// case can afford a fastsincos(): a quarter cycle, so sin climbs from 0
// to 1 as cos falls from 1 to 0, and sin^2 + cos^2 stays 1 the whole way
// across. fastsincos() takes its phase in cycles rather than radians.
//
static void set_mix_pot(struct effect *eff, float m)
{
	eff->mix_pot = m;

	if (eff->mix_law == MIX_POWER) {
		struct sincos w = fastsincos(0.25f * m);
		eff->dry_target = w.cos;
		eff->wet_target = w.sin;
	} else {
		eff->dry_target = 1.0f - m;
		eff->wet_target = m;
	}
}

// How fast the multipliers chase their target: ~10ms at 48kHz
#define MIX_SLEW (1.0f / 512)

//
// Run one effect, and work out how much of it to use.
//
// What this does *not* do is combine the wet signal with the dry one.
// It works out the two multipliers and hands them over, because there
// is no single right way to put two channels back together: a mono
// effect has one answer to spread across both, a panner has a different
// one for each, and a caller that already decided would stop either of
// them from being written.
//
// So the mixing lives in a small per-effect function that gen_effects.py
// writes out next to the effect itself.  For a mono effect - which is
// all of them today - that function reads the left channel, runs the
// effect, and mixes the one result into both, which is exactly what this
// used to do inline.  The difference is that it is now the effect's own
// business rather than something imposed on it from here.
//
static inline sample_t do_effect_step(struct effect *effect, sample_t val)
{
	if (effect->mix != effect->target) {
		int dir = effect->mix < effect->target ? +1 : -1;
		effect->mix += dir;
	}

	if (effect->mix == 0) return val;

	// Chase the mix setting, so moving it doesn't step the gain
	effect->dry += (effect->dry_target - effect->dry) * MIX_SLEW;
	effect->wet += (effect->wet_target - effect->wet) * MIX_SLEW;

	// ...and fade the whole thing in on top of that, which is what
	// 'mix' counting up to 'target' is for now that it no longer
	// carries the setting itself.
	float r = effect->mix * (1.0f / EFF_ENABLE_STEPS);
	float dry = 1.0f + r * (effect->dry - 1.0f);
	float wet = r * effect->wet;

	return effect->step(val, dry, wet);
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

static inline void __audio_func(single_sample)(float mix)
{
	raw_sample_t *cpu_ptr = i2s_dma_buf + cpu_idx;
	cpu_idx = (cpu_idx + 1) & 15;

	// Wait for RX DMA to produce the sample
	while (cpu_ptr == i2s_dma_rx_ptr())
		tight_loop_contents();

	// Check we're safely ahead of TX DMA
	// Missing the deadline is not clipping, even though the LED
	// shows both - see status.h.
	unsigned int tx_idx = i2s_dma_tx_ptr() - i2s_dma_buf;
	if (((cpu_idx - tx_idx) & 15) < 2)
		samples_dropped++;

	// In-place processing
	raw_sample_t sample = *cpu_ptr;
	sample_t in = process_input(sample);
	sample_t usb_in = get_usb_audio_input();

	// We need to do the USB input as stereo too
	if (settings.usb_input == USB_IN_PRE_FX) {
		in.left += usb_in.left;
		in.right += usb_in.right;
	}

	//
	// The signal chain proper: trim and the gate, then whatever is
	// routed.  Called straight rather than through do_effect_step()
	// because it is not an effect - see effects/signal_chain.h.
	//
	sample_t out = signal_chain_step(in);
	for (int i = 0; i < routed_effect_count; i++) {
		out = do_effect_step(effects[effect_chain[i]], out);
	}

	// ...and the far end of it.  Slewed by signal_chain_step() above.
	out.left *= signal_chain.volume;
	out.right *= signal_chain.volume;

	//
	// Global bypass crossfades to the untouched input, so trim, the
	// gate and the volume all go away with everything else.  Bypass
	// means bypass, which does mean it can be a step in level.
	//
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

static __attribute__((noinline)) void __audio_func(make_one_noise)(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];

		unsigned seq = smp_load_acquire(&effect->seq);
		if (seq == effect->last)
			continue;

		//
		// An effect that isn't running doesn't need its
		// coefficients recomputed - but don't mark the update as
		// consumed either, or it just gets lost.  'target' is
		// already set by the time an effect is routed back in, so
		// this picks the change up before it can be heard.
		//
		// 'no_mix' is exempt because "isn't running" is a
		// statement about the wet/dry fade, and such an effect
		// isn't part of it.  Both cases need it: the settings
		// pseudo-effect is nothing *but* its init(), and used to
		// force 'target' nonzero from inside init() purely to
		// keep this test from skipping it next time; and the
		// signal chain runs unconditionally, so its coefficients
		// always matter.
		//
		if (!effect->no_mix && !effect->mix && !effect->target)
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
