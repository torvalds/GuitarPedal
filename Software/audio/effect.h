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

//
// A pot's raw value turned into whatever the pot is measured in.  Takes
// the whole array rather than the one byte, and knows its own index -
// so a call site names the pot once and cannot pair the accessor of one
// with the position of another.
//
typedef float (*pot_convert_fn)(const unsigned char *);

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

	//
	// What saved state is matched against.  Both are computed by
	// gen_effects.py and compiled in, so nothing here ever hashes
	// anything - loading a scene compares two words per effect.
	//
	// 'id_hash' says *which effect this is*, and comes from the
	// short name, which is already required to be unique and is
	// already the effect's name in this generated code.  Rename it
	// and this effect's saved state stops being found, which is the
	// price of not asking people to hand out permanent numbers.
	//
	// 'pot_hash' says *what its pots meant*, and covers their
	// labels, curves, ranges and enumerations in order.  Not their
	// defaults or units: changing a default does not change what an
	// already-stored value means, and wiping everybody's scenes over
	// a retuned default would be a poor trade.
	//
	uint32_t id_hash, pot_hash;

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

	//
	// Set by 'MIX: STEREO': this effect reads both channels itself,
	// so the input half of 'channels' does not apply to it.  There
	// is nothing to select when it wanted both anyway.
	//
	unsigned char stereo;

	//
	// Which channels this effect reads and writes, and how much of
	// the channel it did not write survives a merge.
	//
	// A property of the effect exactly like 'mix', and like 'mix' it
	// is applied entirely by do_effect_step() - no effect knows any
	// of this exists.  Zero is "read left, write both", which is
	// what every effect did before there was a choice.
	//
	unsigned char channels;
	float merge;

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
	sample_t (*step)(sample_t);
	const struct pot_descr pots[10];
};

#define EFFECT_POT(...) { __VA_ARGS__ }

//
// The largest value this pot can hold.
//
// 120 for anything continuous - see POT_TO_FLOAT() - but an enumeration
// only has as many valid values as it has names, and a stored value
// past the end of the list would index a NULL.  A pot with no label is
// not a pot at all and holds nothing.
//
static int max_pot_val(struct effect *effect, int pot)
{
	const struct pot_descr *desc = effect->pots + pot;
	const char *const *enums;

	if (!desc->label)
		return 0;

	enums = desc->enum_names;
	if (!enums)
		return 120;

	for (int i = 0; ; i++) {
		if (!enums[i])
			return i - 1;
	}
}

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
// Which channels an effect reads and writes.
//
// Two 2-bit fields in one byte, and zero is what every effect did
// before there was a choice: read the left channel, write both.  So an
// effect that has never been told otherwise behaves exactly as it did,
// and a saved scene full of zeroes means the same thing it always did.
//
#define CH_IN(c)	((c) & 3)
#define CH_OUT(c)	(((c) >> 2) & 3)

enum ch_in {
	CH_IN_LEFT,
	CH_IN_RIGHT,
	// 2 and 3 reserved.  A "sum of both" input is the one thing the
	// output modes below cannot already express - read both, write
	// one, keep the other for a later merge - and it is reserved
	// rather than written so that adding it is not a format change.
};

enum ch_out {
	CH_OUT_BOTH,		// the answer goes everywhere
	CH_OUT_LEFT,		// ...to the left, right keeps what it had
	CH_OUT_RIGHT,
	CH_OUT_MERGE,		// answer plus what was kept, to both
};

//
// How a parameter is named, by SysEx and by a binding alike.
//
// Zero is the mix and 1..10 are the effect's own pots, which is the
// numbering the app and the rule table have always used.  The three
// above that are the same kind of thing as the mix rather than the same
// kind of thing as a pot: properties of how an effect is wired into the
// chain rather than of what it does to a sample, shared by every effect
// and declared by none of them.
//
// Addressed as pots because that is what makes them reachable.  The one
// place that decides what a number means - set_target() - is also what
// bindings go through, so a footswitch can put an effect on the left and
// another one on the right without anything here knowing about
// footswitches.  A command of their own would have needed teaching to
// SysEx and to bindings separately, and set_effect_mix() already has the
// comment about where that leads.
//
#define POT_MIX		0
#define POT_LAST	10
#define POT_CH_IN	11
#define POT_CH_OUT	12
#define POT_MERGE	13
#define POT_MAX		POT_MERGE

//
// Run one effect, and work out how much of it to use, and where it goes.
//
// The effect itself knows none of this.  It is handed a sample and hands
// one back; everything about which channel it came from, how much of it
// to use and where to put the answer happens here - the same bargain
// 'mix' already had, extended to say where as well as how much.
//
// The wet and dry are still blended against the channel being *written*,
// using that channel's own prior value.  Which means an effect writing
// to one side leaves the other exactly alone, and that is what lets a
// split survive several effects before anything merges it.
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

	unsigned int in = CH_IN(effect->channels);
	unsigned int out = CH_OUT(effect->channels);

	//
	// A mono effect reads whatever is in the left half, so feeding it
	// the right channel means putting it there.  A stereo one already
	// wanted both and is handed the sample untouched.
	//
	sample_t fed = val;
	if (!effect->stereo && in == CH_IN_RIGHT)
		fed.left = val.right;

	sample_t got = effect->step(fed);

	switch (out) {
	case CH_OUT_LEFT:
		val.left = dry * val.left + wet * got.left;
		return val;

	case CH_OUT_RIGHT:
		val.right = dry * val.right + wet * got.right;
		return val;

	case CH_OUT_MERGE: {
		//
		// The answer, blended against the channel it came from,
		// plus whatever weight of the channel it did not touch.
		// At unity that is a plain sum, so a signal split in two
		// and put back together with nothing in between comes
		// back at unity - which is the property that makes a
		// split path worth having at all.
		//
		float src = in == CH_IN_RIGHT ? val.right : val.left;
		float kept = in == CH_IN_RIGHT ? val.left : val.right;
		float here = in == CH_IN_RIGHT ? got.right : got.left;

		val.left = val.right = dry * src + wet * here +
				       effect->merge * kept;
		return val;
	}

	default:
		val.left = dry * val.left + wet * got.left;
		val.right = dry * val.right + wet * got.right;
		return val;
	}
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

//
// Metering, at the two ends of the chain.
//
// A 'struct envelope' with a fast attack and a slow release is a peak
// meter, so there is nothing to write here beyond picking the times - and
// the same thing with those reversed is a floor follower, which is what
// the noise floor estimate is.  Stop playing and the peak falls to the
// noise in a third of a second while the floor follows it down; start
// playing and the peak jumps while the floor takes five seconds to
// notice.
//
// The floor is fed the *gate's* envelope and nothing else, and the reason
// is definitional rather than measured: a floor reading is only useful if
// it can be compared against 'Gate', Gate is compared against the
// gate's envelope, so the floor has to be that same envelope.  Anything
// else is a different quantity wearing the same units, and would only
// happen to agree.
//
// How far the two *would* diverge is not known.  In theory it depends on
// the character of the noise rather than its level: the peak meter's
// 0.1ms attack catches transients that the gate's 1.5ms one averages
// away, so tonal hum should read the same on both and broadband noise
// should read higher on the peak meter by something like its crest
// factor.  That is theory.  Nothing here has measured it, and an earlier
// version of this comment claimed nine decibels on the strength of two
// readings that turned out to be from different boards.
//
// What has been measured, for scale (see process.h for the reference):
//
//	90mVpp at 110Hz reads -30dBFS on both boards, which is also
//	the arithmetic: 31.8mVrms against 1Vrms is -29.95dB.  So the
//	analog front ends agree, and dBFS means the same thing on each.
//
//	the noise floor reads -61dBFS on a current board and -56dBFS
//	on an early one.  Boards differ; the meter is measuring that.
//
//	on each board the floor now sits within a couple of dB of the
//	Level at which the gate actually starts closing, which is the
//	property this is for.
//
static struct envelope meter_in_env, meter_floor_env, meter_out_env;

static void init_meters(void)
{
	envelope_init(&meter_in_env, 0.1f, 300.0f);
	envelope_init(&meter_floor_env, 5000.0f, 100.0f);
	envelope_init(&meter_out_env, 0.1f, 300.0f);
}

static inline void __audio_func(single_sample)(float mix)
{
	raw_sample_t *cpu_ptr = i2s_dma_buf + cpu_idx;
	cpu_idx = (cpu_idx + 1) & 15;

	//
	// Wait for RX DMA to produce the sample.
	//
	// This loop is the idle time, by definition: per sample period the
	// cpu either spins here or works, and the period is known because
	// the DMA sets it.  So timing the spin measures the load exactly,
	// with nothing to estimate.
	//
	// One sample in sixteen, because two bus reads at 48kHz to measure
	// a load is about a percent of the thing being measured.  Sampling
	// it costs a sixteenth of that and the answer is averaged anyway.
	//
	static unsigned int meter_phase;
	bool timed = !(++meter_phase & 15);
	uint32_t spin_start = timed ? timer_hw->timerawl : 0;

	while (cpu_ptr == i2s_dma_rx_ptr())
		tight_loop_contents();

	if (timed) {
		//
		// 20.83us per sample at 48kHz.  Idle longer than that means
		// the previous sample finished early and this one had not
		// arrived yet, which is still idle.
		//
		float idle = (timer_hw->timerawl - spin_start) * (SAMPLES_PER_SEC / 1000000.0f);
		float load = idle < 1.0f ? 1.0f - idle : 0.0f;

		meter_load += (load - meter_load) * (1.0f / 64);
	}

	//
	// Check we're safely ahead of TX DMA.  Missing the deadline is not
	// clipping, even though the LED shows both - see status.h.
	//
	// This comparison is a race and is meant to be one.  One side is a
	// counter this loop keeps, the other is where a DMA engine has got
	// to, and nothing synchronises them - nothing can, which is the
	// point: if the cpu keeps up they stay apart, and if it doesn't
	// they don't.  So this is a stochastic detector.  Fall behind and
	// it comes out true *sometimes*, at a rate that says how far
	// behind, not every sample and not on the same ones twice.
	//
	// Which makes 'samples_dropped' a rate estimate rather than a
	// tally, and is why nothing here tries to make it exact.  The
	// increment is not atomic against the drain in send_status(), so a
	// reset can be lost and the count read high for a tick; against a
	// number that has no exact value to begin with, that is not worth
	// an ldrex/strex pair.  Rather like the clipping indicator: a
	// signal is clipping even though it is only over the line some of
	// the time, because a signal goes up and down.
	//
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
	// Metered here rather than inside the signal chain, because this is
	// the pedal's input: what arrived, before Trim has an opinion.
	//
	meter_in = envelope_step(&meter_in_env, in.left);

	//
	// The signal chain proper: trim and the gate, then whatever is
	// routed.  Called straight rather than through do_effect_step()
	// because it is not an effect - see effects/signal_chain.h.
	//
	sample_t out = chain_step(in);

	//
	// ...and the floor after it, because it follows the gate's own
	// envelope rather than the peak meter above.  Same quantity 'Gate'
	// is compared against, so the two numbers can be read against each
	// other - which is the only reason to show a floor at all.
	//
	meter_floor = envelope_step(&meter_floor_env, chain.envelope.value);

	for (int i = 0; i < routed_effect_count; i++) {
		out = do_effect_step(effects[effect_chain[i]], out);
	}

	// ...and the far end of it.  Slewed by chain_step() above.
	out.left *= chain.volume;
	out.right *= chain.volume;

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

	//
	// And the output meter, on what actually leaves - after Volume and
	// after the bypass crossfade, so a bypassed pedal reads its input.
	//
	float peak = fabsf(out.left) > fabsf(out.right)
		   ? fabsf(out.left) : fabsf(out.right);
	meter_out = envelope_step(&meter_out_env, peak);

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
