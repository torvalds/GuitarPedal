// NAME: Expression Jack [EXPJACK]
// PRIORITY: 131
// MIX: NONE		// it isn't an effect, and there is nothing to mix
//
// Kept once rather than per scene: what is plugged into the jack is
// true of the pedal and not of any one sound.
//
// GLOBAL
//
// Whether the pedal is allowed to answer the next question itself.
//
// Manual is the original bargain and is still the default: a probe runs
// when a host asks and never otherwise, and what it finds is a proposal
// rather than something acted on.  That is what lets you say "a treadle
// is coming later" while sitting at a desk with nothing plugged in.
//
// Auto reports what is out there instead, and the difference is the
// point: it says "a treadle is coming later" back to you as Nothing,
// because nothing is what is on the jack.  It still never overwrites an
// answer it cannot better - a reading consistent with what is set
// confirms it, and "cannot tell" changes nothing at all - but a setting
// it *can* better it will, including the one it started up with.
//
// So the two are not degrees of the same thing.  Manual holds what you
// tell it; Auto tells you what is there.
//
// POT: "Detect" ENUM(Manual Auto) = Manual
// INFO: Auto notices what you plug in and unplug, and will overrule a
// INFO: setting that does not match. It waits for the readings to stop
// INFO: moving before it decides, so push a plug all the way home in one
// INFO: go: one left half in is a jack with an open tip and an open
// INFO: ring, which is a footswitch, and it is not being fooled so much
// INFO: as told. Unplug and replug it properly if that happens.
//
// What is out there.  In Manual this is the setting; in Auto it is both
// that and the readout, because saying what is on the jack by hand is
// also how you tell Auto to stop guessing.
//
// POT: "Accessory" ENUM(Nothing Footswitches Expression Stomp+LED) = Nothing
// INFO: What is on the jack. Auto asks the pedal to look, and writes
// INFO: what it found here; you can also just say.
//
// Everything below this is about a treadle and does nothing without one,
// so each of them says so and the app can stop drawing them.  The pedal
// has always ignored them - exp_calibrate_task() gives up unless the
// accessory is a treadle - which is exactly the kind of agreement that
// only one half of ever gets updated, so it is written down once here
// and both halves read it.
//
// Which contact an expression pedal drives, since the two conventions
// disagree and a plug cannot say which it is.  Getting it wrong still
// sweeps the full range, so "does it move" cannot pick between them -
// what the wrong one loses is the shape, reaching 92% of its range by
// half travel.
//
// POT: "Type" ENUM(Roland Yamaha) = Roland
// NEEDS: ACCESSORY = Expression
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
// Opened by hand and closed by either: the window shuts itself once a
// swing has added nothing to what it knows, which is a different claim
// from "it opens itself" and leaves the ratchet argument intact.
//
// POT: "Calibrate" BOOL = Off
// NEEDS: ACCESSORY = Expression
// INFO: Switch this on and rock the treadle end to end a few times. It
// INFO: switches itself off once a swing has taught it nothing new, and
// INFO: you can switch it off yourself at any point. Nothing else moves
// INFO: the range, so unplugging the pedal or standing on it cannot
// INFO: spoil a setting that works.
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
// NEEDS: ACCESSORY = Expression
// POT: "Toe" LINEAR(40 100) = 100 %
// NEEDS: ACCESSORY = Expression
//
// Which pots the app has to be able to find rather than merely show: one
// so that Auto can offer what the probe found, and one so that switching
// calibration on can say what to do next.  By role, because a label is
// what a person reads and is free to change.
//
// ROLE: EXPJACK:ACCESSORY
// ROLE: CALIBRATE:CALIBRATE
//
// The expression jack pseudo-effect - what is on it, and how to read it

struct {
	int detect;
	int accessory;
	int type;
	int learning;
	int heel, toe;		// in raw converter counts, not percent
} expression;

static void expjack_init(unsigned char pot[10])
{
	expression.detect = pot[EXPJACK_DETECT];
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
