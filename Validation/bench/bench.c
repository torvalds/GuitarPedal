//
// The pedal's audio core, on a workstation.
//
// This is not a model of the signal path.  It is the signal path: the
// same audio/effect.h, the same effects/*.h, the same single_sample(),
// compiled with the same flags for a different instruction set.  The
// only things replaced are the two register blocks the audio core
// touches, and they are replaced with ordinary memory - see shim/.
//
// That distinction is the whole reason this exists.  A harness that
// re-implemented the middle of single_sample() would be a second
// opinion about what the pedal does, and a second opinion is exactly
// what you cannot check an effect against.
//
// Raw float32 stereo frames in on stdin, the same out on stdout, so
// every signal and every measurement belongs to the python on the other
// end.  Effects are named, pots are named, and nothing here is
// addressed by a number that could quietly come to mean something else.
//
// Build: see ../Makefile.  It needs effect_map.h and the three math
// tables generated first, which the Makefile does into bench/gen/.
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

//
// The two fake register blocks the shim headers declare.  Defined here
// rather than in a header of their own because they are storage, and
// this is the only translation unit there is - the same bargain the
// firmware makes with pedal.c.
//
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

struct bench_dma bench_dma_regs;
struct bench_timer bench_timer_regs;

//
// Everything below is pedal.c's include list, in pedal.c's order,
// stopping at the point where the pedal starts being a pedal.  Include
// order is program order in this codebase, so the order matters and the
// omissions are deliberate: no tinyusb, no i2c, no PIO, no scene
// storage, no UI.  None of them is between an input sample and an
// output one.
//
#include "status.h"

#include "Audio/types.h"
#include "Audio/util.h"
#include "Audio/envelope.h"
#include "Audio/single-pole.h"
#include "Audio/biquad.h"
#include "Audio/fft.h"
#include "Audio/analyze.h"

//
// Two of the things audio/effect.h expects to already exist - see the
// contract at the top of it.  Only tuner_mode is reachable from the
// audio path, and process_input() diverts the whole signal into the
// tuner when it is set, so it is held at zero here for the same reason
// the tuner is not built: a bench measuring an effect is not measuring
// the tuner.  user_interaction is written by hardware.h and read by
// nothing on this side.
//
static int tuner_mode = 0;
static volatile int user_interaction = 0;

#include "Audio/effect.h"

uint8_t effect_chain[MAX_ROUTED_EFFECTS];
uint8_t routed_effect_count = 0;

#include "effect-state.h"

//
// Declared by audio/effect.h and defined by usb-audio.h on the pedal.
// There is no USB here, and USB audio input is a separate question from
// what an effect does to a signal, so it is silence.
//
sample_t get_usb_audio_input(void)
{
	sample_t zero = { 0.0f, 0.0f };
	return zero;
}

static void die(const char *fmt, ...)
	__attribute__((format(printf, 1, 2), noreturn));

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

//
// Naming things
//
// Effects are addressed by their display name and pots by their label,
// because those are the two strings that actually exist at runtime.
// The short name - [ECHO], [CHAIN] - is deliberately not in struct
// effect: it is the effect's name in the generated C, and
// audio/effect.h says outright that storing it "cost a pointer per
// effect for nobody".  Adding one so a test could say ECHO would be
// paying that cost after all, to spell a name the display string
// already spells.
//
// Matching is case-insensitive, exact first and then unique substring,
// so 'echo' finds "Tape Echo" while an ambiguous fragment is an error
// with the candidates in it rather than a guess.
//
static bool name_hit(const char *haystack, const char *needle, bool *exact)
{
	*exact = !strcasecmp(haystack, needle);
	if (*exact)
		return true;
	for (const char *p = haystack; *p; p++) {
		if (!strncasecmp(p, needle, strlen(needle)))
			return true;
	}
	return false;
}

//
// Labels may carry leading spaces for indentation in the app -
// settings.h has "  ATTN" - so a name on a command line should not have
// to reproduce the layout.
//
static const char *unpad(const char *s)
{
	while (*s == ' ')
		s++;
	return s;
}

static struct effect *find_effect(const char *name)
{
	struct effect *hit = NULL;
	int hits = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(effects); i++) {
		bool exact;

		if (!name_hit(effects[i]->name, name, &exact))
			continue;
		if (exact)
			return effects[i];
		hit = effects[i];
		hits++;
	}

	if (hits == 1)
		return hit;

	fprintf(stderr, hits ? "'%s' is ambiguous.  It could be:\n"
			     : "There is no effect called '%s'.  There is:\n",
		name);
	for (int i = 0; i < (int)ARRAY_SIZE(effects); i++) {
		bool exact;

		if (!hits || name_hit(effects[i]->name, name, &exact))
			fprintf(stderr, "    %s\n", effects[i]->name);
	}
	exit(1);
}

static int find_pot(struct effect *e, const char *label)
{
	int hit = -1, hits = 0;

	for (int i = 0; i < 10; i++) {
		bool exact;

		if (!e->pots[i].label)
			continue;
		if (!name_hit(unpad(e->pots[i].label), label, &exact))
			continue;
		if (exact)
			return i;
		hit = i;
		hits++;
	}

	if (hits == 1)
		return hit;

	fprintf(stderr, hits ? "'%s' is ambiguous on %s.  Its pots are:\n"
			     : "%s has no pot called '%s'.  It has:\n",
		hits ? label : e->name, hits ? e->name : label);
	for (int i = 0; i < 10; i++) {
		if (e->pots[i].label)
			fprintf(stderr, "    %s\n", unpad(e->pots[i].label));
	}
	exit(1);
}

static unsigned char pot_value(struct effect *e, int idx, const char *text)
{
	char *end;
	long v = strtol(text, &end, 0);

	if (end == text || *end)
		die("'%s' is not a number\n", text);
	if (v < 0 || v > max_pot_val(e, idx))
		die("%s / %s takes 0..%d, not %ld\n",
		    e->name, unpad(e->pots[idx].label), max_pot_val(e, idx), v);
	return (unsigned char) v;
}

//
// --list, which is how you find out what the other options accept.
//
static void list_effects(void)
{
	for (int i = 0; i < (int)ARRAY_SIZE(effects); i++) {
		struct effect *e = effects[i];

		printf("%s%s [mix: %s]\n", e->name,
		       effect_always_runs(i) ? "  (always runs)" : "",
		       e->no_mix ? "none" :
		       e->mix_law == MIX_POWER ? "equal power" : "linear");
		for (int p = 0; p < 10; p++) {
			if (!e->pots[p].label)
				continue;
			printf("    %-14s 0..%-4d default %-4d %s\n",
			       unpad(e->pots[p].label), max_pot_val(e, p),
			       e->pots[p].def_val,
			       e->pots[p].unit ? e->pots[p].unit : "");
		}
	}
}

//
// The two ends of the converter, run backwards and forwards.
//
// process_input() and convert_output() are deliberately not inverses of
// each other - the 1.2198 asymmetry in audio/process.h is exactly what
// makes the peak of the internal float equal the RMS volts of a sine -
// so undoing one with the other would put a silent 1.7dB in the middle
// of every measurement.  Each is undone with itself.
//
// So stdin and stdout are both in the pedal's *internal* float scale,
// where 1.0 is one volt RMS of sine and every dB an effect is marked in
// is measured.  A transparent chain is then a bit-exact pipe, which is
// the property the negative control checks.
//
// The input can carry a little over full scale (2^31 x the multiplier
// is 1.2198) and the output cannot: convert_output() pins anything at
// or past 1.0 and says so in 'output_clipped'.  That asymmetry is the
// pedal's, not the bench's, and the summary on stderr is how a
// measurement finds out it happened.
//
#define RAW_FULL_SCALE 2147483647.0f

static s32 float_to_raw(float v)
{
	float scaled = v / (float)SAMPLE_TO_FLOAT_MULTIPLIER;

	if (scaled >= RAW_FULL_SCALE)
		return INT32_MAX;
	if (scaled <= -RAW_FULL_SCALE)
		return INT32_MIN;
	return (s32) scaled;
}

static float raw_to_float(s32 v)
{
	return v * (float)(1.0 / 2147483648.0);
}

//
// Asking a primitive directly.
//
// Everything above runs a signal through the audio path.  This does not:
// it maps one float to one float through a single function out of
// audio/util.h, so that the fast approximations can be characterised
// against double-precision arithmetic without an effect's own
// distortion sitting on top of the answer.
//
// It is deliberately a separate mode with a separate wire format - mono
// floats rather than stereo frames - because it is not audio and should
// not be mistaken for it in a capture file.
//
// Note that the sine here is fastsincos(), which is not the same code as
// the sine the test tone generates: lfo_step() interpolates the quarter
// table itself.  Two implementations of one idea, so both are worth
// asking, and the tone generator is asked by running [TESTTONE].
//
static float map_tanh(float x) { return tanhf(x); }
static float map_sin(float x) { return fastsincos(x).sin; }
static float map_cos(float x) { return fastsincos(x).cos; }
static float map_pow2(float x) { return pow2(x); }
static float map_log2(float x) { return log2f(x); }
static float map_exp(float x) { return expf(x); }
static float map_db(float x) { return db_to_level(x); }
static float map_dbA(float x) { return db_to_A(x); }
static float map_tc(float x) { return time_constant(x); }

//
// The two single-pole helpers hand back a one-field struct, so the
// field is what there is to look at: 'alpha' is the whole filter.
//
static float map_sp_freq(float x) { return single_pole_freq(x).alpha; }
static float map_sp_time(float x) { return single_pole_time(x).alpha; }

//
// Not an approximation at all, and here because the pot curves are the
// one place where being exactly right at the ends matters - a pot that
// cannot reach its own declared maximum is a different bug from an
// inaccurate one.
//
static float map_pot_linear(float x) { return POT_TO_FLOAT(x); }
static float map_pot_cubic(float x) { return cubic(POT_TO_FLOAT(x), 0.0f, 1.0f); }

static const struct {
	const char *name;
	float (*fn)(float);
} maps[] = {
	{ "tanh", map_tanh },
	{ "sin", map_sin },
	{ "cos", map_cos },
	{ "pow2", map_pow2 },
	{ "log2", map_log2 },
	{ "exp", map_exp },
	{ "db_to_level", map_db },
	{ "db_to_A", map_dbA },
	{ "time_constant", map_tc },
	{ "single_pole_freq", map_sp_freq },
	{ "single_pole_time", map_sp_time },
	{ "pot_to_float", map_pot_linear },
	{ "pot_cubic", map_pot_cubic },
};

static int run_map(const char *name)
{
	float (*fn)(float) = NULL;
	float x;

	for (int i = 0; i < (int)ARRAY_SIZE(maps); i++) {
		if (!strcasecmp(maps[i].name, name))
			fn = maps[i].fn;
	}
	if (!fn) {
		fprintf(stderr, "no such primitive '%s'.  There is:\n", name);
		for (int i = 0; i < (int)ARRAY_SIZE(maps); i++)
			fprintf(stderr, "    %s\n", maps[i].name);
		return 1;
	}

	while (fread(&x, sizeof(x), 1, stdin) == 1) {
		float y = fn(x);

		if (fwrite(&y, sizeof(y), 1, stdout) != 1)
			die("short write\n");
	}
	fflush(stdout);
	return 0;
}

//
// One "cpu" sample period.
//
// Everything here is what the DMA engine would have done, and nothing
// here is what single_sample() does - that is the point.  The receive
// pointer goes one slot ahead of the cpu so the spin exits immediately,
// and the transmit pointer stays half a ring behind so the deadline
// check in single_sample() stays false and samples_dropped keeps
// meaning something.
//
static void bench_sample(raw_sample_t in, raw_sample_t *out)
{
	unsigned slot = cpu_idx;

	i2s_dma_buf[slot] = in;

	bench_dma_regs.ch[dma_rx].write_addr = (uintptr_t)&i2s_dma_buf[(slot + 1) & 15];
	bench_dma_regs.ch[dma_tx].read_addr  = (uintptr_t)&i2s_dma_buf[(slot + 8) & 15];

	//
	// 20.83us a sample at 48kHz.  The load meter reads the difference
	// across its own spin, which is zero here, so it will report a
	// fully loaded core.  That is honest - this "core" never waits -
	// and it is not audio either way.
	//
	bench_timer_regs.timerawl += 21;

	single_sample(1.0f);

	*out = i2s_dma_buf[slot];
}

int main(int argc, char **argv)
{
	struct { struct effect *e; int pot; unsigned char val; } pots[64];
	struct { struct effect *e; unsigned char val; } mixes[32];
	struct effect *route[MAX_ROUTED_EFFECTS];
	int nr_pots = 0, nr_mixes = 0, nr_route = 0;
	routing_bitmap_t routable;
	unsigned long frames = 0;

#ifdef __SSE__
	//
	// The pedal sets FZ in FPSCR on both cores - see enable_ftz() in
	// pedal.c - so subnormal results flush to zero.  Do the same here,
	// or a reverb tail decaying past 1e-38 takes a different path on
	// the two machines and the bench stops being the pedal.
	//
	_mm_setcsr(_mm_getcsr() | 0x8040);	/* FTZ | DAZ */
#endif

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		char buf[256], *eq, *colon;

		if (!strcmp(arg, "--list")) {
			list_effects();
			return 0;
		}

		if (i + 1 >= argc)
			die("%s wants a value\n", arg);
		snprintf(buf, sizeof(buf), "%s", argv[++i]);

		//
		// Before any of the effect setup, because it does not use
		// any of it - see run_map().
		//
		if (!strcmp(arg, "--map"))
			return run_map(buf);

		if (!strcmp(arg, "--route")) {
			if (nr_route >= MAX_ROUTED_EFFECTS)
				die("a chain holds %d effects\n", MAX_ROUTED_EFFECTS);
			route[nr_route++] = find_effect(buf);
			continue;
		}

		eq = strchr(buf, '=');
		if (!eq)
			die("%s wants a NAME=VALUE\n", arg);
		*eq++ = 0;

		if (!strcmp(arg, "--mix")) {
			if (nr_mixes >= (int)ARRAY_SIZE(mixes))
				die("too many --mix\n");
			mixes[nr_mixes].e = find_effect(buf);
			mixes[nr_mixes].val = pot_value(mixes[nr_mixes].e, 0, eq);
			nr_mixes++;
			continue;
		}

		if (!strcmp(arg, "--pot")) {
			if (nr_pots >= (int)ARRAY_SIZE(pots))
				die("too many --pot\n");
			colon = strrchr(buf, ':');
			if (!colon)
				die("--pot wants EFFECT:POT=VALUE\n");
			*colon++ = 0;
			pots[nr_pots].e = find_effect(buf);
			pots[nr_pots].pot = find_pot(pots[nr_pots].e, colon);
			pots[nr_pots].val = pot_value(pots[nr_pots].e,
						      pots[nr_pots].pot, eq);
			nr_pots++;
			continue;
		}

		die("unknown option %s\n", arg);
	}

	//
	// Bring the effects up the way pedal.c's init_effects() does:
	// every effect reset to its declared defaults, the routing, the
	// pots, then one init() per effect.  What is skipped is
	// load_globals() and load_scene(), which read an eeprom that is
	// not here - so the starting point is a pedal that has never been
	// saved to, which is exactly the reproducible one.
	//
	for (int i = 0; i < (int)ARRAY_SIZE(effects); i++)
		reset_effect(effects[i]);

	routable = routing_start();
	for (int i = 0; i < nr_route; i++) {
		uint8_t id = 0;

		while (id < EFFECT_COUNT && effects[id] != route[i])
			id++;
		if (!routing_add(&routable, id))
			die("cannot route %s: already routed, or it is not routable\n",
			    route[i]->name);
	}
	routing_end(routable);

	//
	// After the routing, because routing_end() unroutes everything
	// that was not asked for and unroute_effect() puts every pot back
	// to its default on the way past.
	//
	for (int i = 0; i < nr_pots; i++)
		set_effect_pot(pots[i].e, pots[i].pot, pots[i].val);
	for (int i = 0; i < nr_mixes; i++)
		set_effect_mix(mixes[i].e, mixes[i].val);

	for (int i = 0; i < (int)ARRAY_SIZE(effects); i++) {
		struct effect *e = effects[i];

		e->last = e->seq;
		e->init(effect_pots(e));
	}

	init_meters();

	//
	// And then it is just a pipe.  A frame at a time rather than a
	// buffer, because the ring is sixteen slots deep and the point is
	// to stay inside it; the cost is irrelevant next to what python
	// does with the answer.
	//
	for (;;) {
		float frame[2];
		raw_sample_t in, out;

		if (fread(frame, sizeof(frame), 1, stdin) != 1)
			break;

		in.left = float_to_raw(frame[0]);
		in.right = float_to_raw(frame[1]);

		bench_sample(in, &out);
		frames++;

		frame[0] = raw_to_float(out.left);
		frame[1] = raw_to_float(out.right);

		if (fwrite(frame, sizeof(frame), 1, stdout) != 1)
			die("short write\n");
	}

	fflush(stdout);

	//
	// The conditions the numbers were taken under, on stderr where
	// they cannot be mistaken for signal.  'clipped' and 'dropped'
	// are the two that invalidate a measurement rather than merely
	// describing it, and 'fade' is how many frames at the front of the
	// capture are an effect ramping in rather than an effect.
	//
	fprintf(stderr,
		"frames %lu  clipped %u  dropped %u  fade %d\n"
		"in %.6f  floor %.6f  out %.6f\n",
		frames, output_clipped, samples_dropped, EFF_ENABLE_STEPS,
		meter_in, meter_floor, meter_out);

	return 0;
}
