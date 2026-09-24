#ifndef HWTONE_H
#define HWTONE_H

//
// A tone stack in the codec's own biquads.
//
// Three sections - a low shelf, a peak and a high shelf - which is
// exactly what the codec allocates per channel, on the record path and
// again on playback.  Nothing here touches a sample: the filters run in
// the codec whether or not the audio core is doing anything at all.
//
// All of it is core 0's.  The effects declare 'INIT: core0', so the
// bands are worked out by prepare() on the same core that writes the
// coefficients to the codec, and core 1 is left with an activate() that
// has nothing to pick up.
//
// Boosts do not fit the codec's 1.31 coefficients - the largest
// numerator coefficient of a section is its own boost - so the channel
// gives the gain away and its digital volume control puts it back.
// hwtone_task() is where that is worked out.
//

//
// Which set of sections a stack drives.  Not a property of the stack:
// the two are the same filter in the same shape, and all that differs
// is which pages hwtone_task() writes, so whoever drives it says so.
//
enum hwtone_path { HWTONE_RECORD, HWTONE_PLAYBACK };

//
// The three sections, in the order the codec has them.  Which pot
// drives which band is the effect's business; what shape each band is,
// is this file's.
//
enum { HWTONE_BASS, HWTONE_MID, HWTONE_TREBLE };

//
// One band, in what the pots are marked in rather than in coefficients.
//
// Frequency and Q are kept as logs because that is the axis they mean
// something on: an octave is an octave wherever it starts, and it is
// the same reason the pots that set them are exponential.  Gain is
// already logarithmic, being decibels.
//
struct hwtone_band {
	float lfreq;			// log2 Hz
	float lq;			// log2 Q
	float gain;			// dB
};

//
// How fast a band is allowed to move, and how close counts as arrived.
//
// The pot cannot help here.  'Mid Freq' is 120 steps over ten octaves,
// so it moves in semitones however slowly a knob is turned, and a
// semitone is a large enough jump in the poles that the state left over
// from the old filter rings audibly against the new one.  Spacing the
// writes out does not help - measured, 10ms to 250ms apart is the same
// noise - because the transients are not overlapping, they are each
// simply too big.  So the step that matters is the one taken here,
// between what the codec holds and where the pot has got to.
//
#define HWTONE_STEP_US	2000
#define HWTONE_APPROACH	0.08f
#define HWTONE_SNAP	0.01f

struct hwtone {
	struct hwtone_band want[3];	// where the pots say to be
	struct hwtone_band live[3];	// what the codec is holding
	bool live_valid;
	float live_scale[3];		// and what each numerator was divided by
	unsigned long long next_us;	// when it may next be moved
};

//
// What prepare() hands over: one band, in Hz, Q and dB.
//
static inline void hwtone_set_band(struct hwtone *ht, int band,
				   float freq, float q, float gain)
{
	ht->want[band].lfreq = log2f(freq);
	ht->want[band].lq = log2f(q);
	ht->want[band].gain = gain;
}

//
// Where a band should be right now.
//
// Switching the stack off is a gain of 0 dB and nothing else.  A
// section at 0 dB is transparent term for term - the numerator and the
// denominator come out identical - so the band stays where it is and
// stops doing anything, which is both what a tone control does and the
// only way out of the stack that does not move the filter.
//
static inline struct hwtone_band hwtone_target(const struct hwtone *ht,
					       int band, bool running)
{
	struct hwtone_band b = ht->want[band];

	if (!running)
		b.gain = 0.0f;
	return b;
}

//
// One band, one step closer.  True if anything moved, because a band
// that did not is a write not worth making - and not making it is what
// buys the time to take small steps at all.
//
// The step is taken here rather than in the coefficients, and that is
// the whole of why this works.  A straight line between two sets of
// coefficients is not a path between two filters: from flat, both
// denominator terms start at zero, so the poles start out at a quarter
// of the sample rate and walk down, and the band sweeps the spectrum on
// its way to where it was asked for.  Easing what the pot is marked in
// leaves the band where it is and only changes its size.
//
static inline bool hwtone_approach(struct hwtone_band *c,
				   const struct hwtone_band *t)
{
	bool moved = false;

#define HWTONE_NUDGE(f) do {						\
		float d = t->f - c->f;					\
		if (d != 0.0f) {					\
			c->f = fabsf(d) <= HWTONE_SNAP			\
			     ? t->f : c->f + d * HWTONE_APPROACH;	\
			moved = true;					\
		}							\
	} while (0)

	HWTONE_NUDGE(lfreq);
	HWTONE_NUDGE(lq);
	HWTONE_NUDGE(gain);
#undef HWTONE_NUDGE
	return moved;
}

//
// And the filter that band is, which is the only place the three
// sections differ from each other.
//
static inline void hwtone_design(struct biquad_coeff *bq, int band,
				 const struct hwtone_band *b)
{
	float freq = pow2(b->lfreq);
	float q = pow2(b->lq);
	float A = db_to_A(b->gain);

	if (band == HWTONE_BASS)
		_biquad_loshelf(bq, freq, q, A);
	else if (band == HWTONE_MID)
		_biquad_peaking(bq, freq, q, A);
	else
		_biquad_hishelf(bq, freq, q, A);
}

#endif // HWTONE_H
