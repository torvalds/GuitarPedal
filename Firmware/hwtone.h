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

struct hwtone {
	struct hwtone_band want[3];	// where the pots say to be
	struct hwtone_band live[3];	// what the codec is holding
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
