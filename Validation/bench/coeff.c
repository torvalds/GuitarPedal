//
// The pedal's biquad coefficients, on a workstation.
//
// Same bargain as bench.c and for the same reason: this does not
// re-derive the cookbook, it calls the pedal's own biquad.h.  A second
// implementation of _biquad_peaking() would be a second opinion, and a
// second opinion is exactly what you cannot check a filter against.
//
// It exists because 'bench --map' cannot see this class of defect.  That
// maps one float through one function; where a biquad lands is a
// property of how sin(w0) and cos(w0) are combined, which needs the
// constructor itself and no audio path at all.
//
// Lines in on stdin:  <type> <freq> <Q> <dB>
// Lines out on stdout: <type> <freq> <Q> <dB> b0 b1 b2 a1 a2
//
// Everything about what those coefficients then mean - where the corner
// actually is, whether that is near enough - belongs to test-biquad.py
// on the other end.  Nothing here has an opinion about pass or fail.
//
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"

#include "Audio/types.h"
#include "Audio/util.h"
#include "Audio/biquad.h"

//
// db_to_A() lives in util.h and is what every caller of the three gain
// constructors goes through, so the sweep goes through it too.
//
static const struct {
	const char *name;
	void (*fn)(struct biquad_coeff *, float, float);
} plain[] = {
	{ "lpf",	_biquad_lpf },
	{ "hpf",	_biquad_hpf },
	{ "notch",	_biquad_notch_filter },
	{ "bpf",	_biquad_bpf },
	{ "bpf_peak",	_biquad_bpf_peak },
	{ "allpass",	_biquad_allpass_filter },
};

static const struct {
	const char *name;
	void (*fn)(struct biquad_coeff *, float, float, float);
} gained[] = {
	{ "peaking",	_biquad_peaking },
	{ "loshelf",	_biquad_loshelf },
	{ "hishelf",	_biquad_hishelf },
};

int main(void)
{
	char type[32];
	double freq, q, db;

	while (scanf("%31s %lf %lf %lf", type, &freq, &q, &db) == 4) {
		struct biquad_coeff c;
		bool done = false;

		for (unsigned i = 0; i < ARRAY_SIZE(plain); i++) {
			if (strcmp(plain[i].name, type))
				continue;
			plain[i].fn(&c, (float)freq, (float)q);
			done = true;
		}
		for (unsigned i = 0; i < ARRAY_SIZE(gained); i++) {
			if (strcmp(gained[i].name, type))
				continue;
			gained[i].fn(&c, (float)freq, (float)q,
				     db_to_A((float)db));
			done = true;
		}
		if (!done) {
			fprintf(stderr, "no such filter '%s'\n", type);
			return 1;
		}

		//
		// %.9e is float32 round-tripped exactly, so the python on
		// the other end is reading the number the pedal has and
		// not a rendering of it.
		//
		printf("%s %g %g %g %.9e %.9e %.9e %.9e %.9e\n",
		       type, freq, q, db, c.b0, c.b1, c.b2, c.a1, c.a2);
	}
	return 0;
}
