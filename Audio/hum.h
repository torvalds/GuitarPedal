//
// The hum cuts: one peaking section per mains line that needed one, run
// on the input ahead of everything else.
//
// The audio core only runs them.  Core 0 measures the hum, works out
// the set and publishes it (Firmware/hum-learn.h); there are two sets so
// it can build one while the other is in use, and the index is what is
// published.  The filter states carry over a change of set, and a cut
// keeps its slot from one set to the next, so each state stays with its
// frequency; direct form I keeps the step small, since its state is past
// inputs and outputs, not anything scaled by the old coefficients.
//
#define HUM_SECTIONS	8

struct hum_set {
	unsigned int n;
	struct biquad_coeff c[HUM_SECTIONS];
};

static struct {
	struct hum_set set[2];
	unsigned int active;
	bool on;			// [DEHUM] is routed
	struct biquad_state s[2][HUM_SECTIONS];
} hum;

// What [DEHUM]'s Target pot asks for, in -dBFS
static unsigned int hum_want_target;

//
// The cuts run whether [DEHUM] is on or not, and only their output is
// chosen, so switching it does not restart filters that take a second
// to settle.  While the hum is measured they always apply, so the learn
// can be heard working.
//
static inline sample_t hum_step(sample_t in)
{
	const struct hum_set *h = &hum.set[smp_load_acquire(&hum.active) & 1];
	sample_t out = in;

	for (unsigned int i = 0; i < h->n; i++) {
		out.left = _biquad_peaking_step(&h->c[i], &hum.s[0][i], out.left);
		out.right = _biquad_peaking_step(&h->c[i], &hum.s[1][i], out.right);
	}
	return hum.on || analyzer_mode == ANALYZE_HUM ? out : in;
}
