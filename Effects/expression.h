// NAME: Expression Jack [EXPJACK]
// PRIORITY: 131
// MIX: NONE		// it isn't an effect, and there is nothing to mix
//
// Kept once rather than per scene: what is plugged into the jack is
// true of the pedal and not of any one sound.
//
// GLOBAL
//
// What is out there.  The pedal does not decide this for itself: a
// probe blocks nothing but takes 24ms of settling, so it runs when a
// host asks and never on its own, and what it finds is a proposal
// written here rather than something acted on.  This value is what the
// pedal runs from, which is also how you say "a treadle is coming
// later" while sitting at a desk with nothing plugged in.
//
// POT: "Accessory" ENUM(Nothing Footswitches Expression Stomp+LED) = Nothing
// INFO: What is on the jack. Auto asks the pedal to look, and writes
// INFO: what it found here; you can also just say.
//
// Which contact an expression pedal drives, since the two conventions
// disagree and a plug cannot say which it is.  Getting it wrong still
// sweeps the full range, so "does it move" cannot pick between them -
// what the wrong one loses is the shape, reaching 92% of its range by
// half travel.
//
// POT: "Type" ENUM(Roland Yamaha) = Roland
// INFO: Roland and Boss put the wiper on the tip; Yamaha and Korg put
// INFO: it on the ring. If the treadle does everything in the first
// INFO: half of its travel, it is the other one.
//
// Learning a treadle's travel, which is not the same as its full
// scale: a pedal taken at face value gives away the top tenth of
// whatever it drives.
//
// Only while this says so.  A range that widens on its own is a
// ratchet - too wide means the treadle stops reaching its own ends and
// nothing ever narrows it back - so it moves inside a window somebody
// opened and at no other time.  Filtering would not save it either: an
// unplugged jack does not give a spike but a steady zero, held for as
// long as it is unplugged, which any filter eventually believes.
//
// POT: "Calibrate" ENUM(Keep Learning) = Keep
// INFO: Set this to Learning, rock the treadle end to end, and set it
// INFO: back to Keep. Nothing else moves the range, so unplugging the
// INFO: pedal or standing on it cannot spoil a setting that works.
//
// Where that learning ends up, as a percentage of the converter's
// range rather than in counts, because a percentage is a number
// somebody can reason about.
//
// Neither half needs all of it.  At the toe the wiper sits at the
// supply pin, which reads 3.3V x Rp/(Rp + 1k) - about 90% for a 10k
// pot and 50% for a 1k one, which is below anything made.  At the heel
// it sits at sleeve and reads near zero whatever the pot is.  Halving
// each doubles what a 0..120 pot can carry: 0.5% a step.
//
// POT: "Heel" LINEAR(0 60) = 0 %
// POT: "Toe" LINEAR(40 100) = 100 %
//
// Which pot the app has to be able to find rather than merely show, so
// that Auto can offer what the probe found.  By role, because a label
// is what a person reads and is free to change.
//
// ROLE: EXPJACK:ACCESSORY
//
// The expression jack pseudo-effect - what is on it, and how to read it

struct {
	int accessory;
	int type;
	int learning;
	int heel, toe;		// in raw converter counts, not percent
} expression;

static void expjack_init(unsigned char pot[10])
{
	expression.accessory = pot[EXPJACK_ACCESSORY];
	expression.type = pot[EXPJACK_TYPE];
	expression.learning = pot[EXPJACK_CALIBRATE];

	//
	// Kept as counts because that is what the converter gives and
	// what everything downstream compares against; percent is for
	// the person setting it.
	//
	expression.heel = lrintf(expjack_heel_pot(pot) * 40.95f);
	expression.toe = lrintf(expjack_toe_pot(pot) * 40.95f);
}

//
// There is no audio here, and that is the point.  See settings.h, which
// is the same shape and says why at length.
//
static inline sample_t expjack_step(sample_t in)
{
	return in;
}
