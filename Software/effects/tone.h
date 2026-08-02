// NAME: Tone [TONE]
// PRIORITY: 10
// COPIES: 2
// GRAPH: LOSHELF:0.707 PEAKING:MID_Q HISHELF:0.707
// POT: "Bass Freq" EXPONENTIAL(20.0 20480.0) = 200.0 Hz
// POT: "Bass" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Mid Freq" EXPONENTIAL(20.0 20480.0) = 800.0 Hz
// POT: "Mid" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Treble Freq" EXPONENTIAL(20.0 20480.0) = 3000.0 Hz
// POT: "Treble" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Mid Q" EXPONENTIAL(0.3 4.0) = 0.707
//
// A tone stack: bass, mid and treble.
//
// The analogue circuit is a feedback network around one gain stage, and
// its two controls interact through it - which is most of what makes a
// Baxandall sound like a Baxandall rather than like two filters. Digital
// has no such constraint, so this is what the circuit was always trying
// to be: a low shelf and a high shelf in series, independent, and the
// corner frequencies adjustable because there is no reason for them not
// to be.  Four multiply-adds a sample either way.
//
// Worth having next to the five-band EQ rather than instead of it.  The
// EQ is the better instrument and the worse tool: most of the time the
// answer is "a bit less low end and a bit more air", and reaching for
// five bands to say that means deciding four things you did not want to
// think about.  This is the same picture with three nodes in it.
//
// The mid is a peaking band, and it is here because two shelves cannot
// make a hump.  A low shelf lifts everything below a corner and a high
// shelf everything above one, so the only way to raise the middle with
// two of them is to overlap opposing shelves and let their skirts add -
// which works, and is a trick rather than a control.  Every guitar tone
// stack ever built is bass/mid/treble for this reason.
//
// The shelves are fixed at Q 0.707.  The mid's is a pot, because a mid
// is the one band where width is a decision rather than a default: 0.707
// is about two octaves, the broad "more body" a tone control is for,
// while the narrow end reaches far enough to pull a single resonance out
// of a boxy guitar.  It reads as Q rather than as a width because that
// is what it is, and the graph shows what it does the moment it moves.
//
// The GRAPH: line names Mid Q, and that is the only place the connection
// is made - the generator writes the Q out from that declaration, so the
// number the app draws with and the number the filter is built from
// cannot be different ones.
//
// There are two of these, which is what 'COPIES: 2' above asks for.
// Two rather than one because an effect owns one set of state, so
// routing the same one twice would run a filter through its own delay
// line and produce nonsense - and one file rather than two copies
// because twins that are edited separately stop being twins.
//
// The copies differ in exactly one thing, which is that each has its
// own state.  So the generator emits the pot accessors and the Q table
// once and both copies share them - they are pure functions of the pot
// array and two of each would only be two things to keep in step - and
// generates just the state, the init and the step per copy.  Those
// three are the only names here that cannot be written down, because
// this file does not know which copy it is being included as; SELF()
// is how it refers to them, and the generator supplies the name at
// each include.
//
// This used to be a symlink, tone2.h pointing here, and the second
// copy existed only in a directory listing.  Saying it in the file is
// better mostly because it is visible from inside the file.
//
// Which one goes where is not decided here either.  They are ordinary
// routable effects, so put one at the front, or one at the back, or
// both, or neither.  Unrouted they cost exactly nothing.
//
// A shelf at 0dB is not approximately transparent, it is exactly
// transparent: with a gain of 1 the numerator and denominator of the
// section come out identical term by term.  So there is no need to
// detect the flat case and skip it, and no click when it stops being
// flat, and the default of flat/flat is a genuine no-op that still
// leaves the filter's state warm.
//
// 0.707 rather than the EQ's 1.0 on the shelves.  A shelf at Q=1
// overshoots slightly before it turns over, which is useful when aiming
// a band at something and wrong when tilting the whole top or bottom of
// a signal.

static struct {
	struct biquad bass, mid, treble;
} SELF(_state);

static void SELF(_init)(unsigned char pot[10])
{
	float q[3];

	tone_graph_q(q, pot);
	biquad_lowshelf(&SELF(_state).bass, tone_bass_freq_pot(pot), q[0],
			db_to_A(tone_bass_pot(pot)));
	biquad_peaking(&SELF(_state).mid, tone_mid_freq_pot(pot), q[1],
		       db_to_A(tone_mid_pot(pot)));
	biquad_highshelf(&SELF(_state).treble, tone_treble_freq_pot(pot), q[2],
			 db_to_A(tone_treble_pot(pot)));
}

static float SELF(_step)(float in)
{
	float val = biquad_step(&SELF(_state).bass, in);
	val = biquad_step(&SELF(_state).mid, val);
	return biquad_step(&SELF(_state).treble, val);
}
