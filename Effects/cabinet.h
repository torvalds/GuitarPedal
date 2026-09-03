// NAME: Cabinet [CAB]
// PRIORITY: 129
// POT: "Cabinet" ENUM(Small-Combo American-1x12 British-4x12 Modern-4x12 Bass-15) = Modern-4x12
// INFO: Which speaker this is.  The list runs small-and-early to
// INFO: big-and-clean rather than being five flavours: the first one
// INFO: gives up almost as soon as you hit it, the last one barely
// INFO: does at all.  Everything a speaker knows about itself is in
// INFO: here - how heavy the cone is, how much wire is on the coil,
// INFO: where it starts to run out of travel, how quickly it cooks.
// POT: "Drive" LINEAR(0.0 30.0) = 15.0 dB
// INFO: How hard the amplifier pushes the cone.  This is the only
// INFO: control that decides whether anything here is audible: fully
// INFO: down the cone barely moves, the model is linear, and it is
// INFO: exactly a filter.  Up from there it compresses, then the bass
// INFO: starts modulating everything else, then it is in breakup.
// INFO: Where on the travel that happens depends entirely on how hot
// INFO: what is in front of it is, which is why the range is wide:
// INFO: the top is not an extreme setting, it is a quiet pickup with
// INFO: nothing in front of it.
// POT: "Resonance" LINEAR(0.0 24.0) = 4.0 ohm
// INFO: The amplifier's output impedance, and it does what the knob of
// INFO: this name does on a valve amp: fills in the low end.  A
// INFO: solid-state amp is near zero ohms and holds the cone still; a
// INFO: valve amp is several ohms and lets the driver's own impedance
// INFO: peak push the voltage back up where it resonates.  That bump
// INFO: is the amp and the speaker arguing, and it is the reason the
// INFO: two are not separable.
// POT: "Axis" LINEAR(0.0 1.0) = 0.5
// INFO: Where the microphone is pointing.  Straight at the dust cap is
// INFO: bright and beats out the speaker's bite; out at the edge of
// INFO: the cone is dark and soft.  One control rather than two
// INFO: because a real driver does not separate them - the same
// INFO: breakup that makes the bite is what carries the top end - so
// INFO: this moves the presence peak and the rolloff together.

/*
 * A guitar speaker as a moving mass, rather than as a filter.
 *
 * ====================================================================
 * WHAT THIS IS FOR, BECAUSE IT IS NOT A BETTER SET OF BIQUADS
 * ====================================================================
 * The small-signal behaviour of a loudspeaker is a *linear* third-order
 * system - the Thiele-Small lumped model:
 *
 *	e        = i*Re + Le*di/dt + Bl*v	(electrical, with back-EMF)
 *	Mms*dv/dt = Bl*i - Rms*v - K*x		(mechanical)
 *	dx/dt    = v ;   pressure is proportional to dv/dt
 *
 * Discretise that and you have a biquad and a one-pole wearing physical
 * parameter names.  The cab simulator this replaced had a 75Hz highpass
 * and a 110Hz bandpass that already *were* that resonance, tuned by
 * hand, and there was nothing to be won by deriving them from Mms and
 * Cms instead.  Worse, a rigid piston has no 5kHz cliff and no presence
 * peak - those come from the cone giving up and ceasing to move as one
 * piece - so a physical model of the *piston* gets the top end wrong
 * rather than merely no better.
 *
 * So the integrator here is the part a filter cannot be, and only that
 * part: Bl(x), K(x) and Re(T) are level- and history-dependent, which
 * no filter can be - and neither can an impulse response, which is a
 * linear model too.
 *
 * That is also the whole of what the bake-off turned on.  Fitted
 * against the same driver curve with the same filter budget, the two
 * approaches tied on frequency response - 1.86dB rms against 1.89 - and
 * response decided nothing.  What decided it was *where the distortion
 * lands*: at matched breakup on real strummed guitar this puts 19.5dB
 * less added energy above 3kHz, because excursion falls as 1/f^2 and so
 * an excursion-driven nonlinearity distorts down where the excursion
 * is.  A memoryless clipper has no such preference, and the old one
 * measured +3.1dB in the top octave - adding more than the signal it
 * was given, which is precisely the fizz a cab sim exists to remove.
 *
 * ====================================================================
 * WHY THERE ARE FILTERS HERE ANYWAY
 * ====================================================================
 * The last four biquads in cab_step() are a scoop, a presence peak and
 * a fourth-order cliff.  They are not part of the model and they are
 * not pretending to be: they are the cone *bending*, which a piston
 * does not do, plus the baffle and the box, which are not in here at
 * all.  This is where the cabinet actually lives; below them is a
 * driver in free air.
 *
 * Their numbers are not the old cab sim's, and copying them was a
 * mistake worth recording.  That file had no inductance, so its cliff
 * was placed to stand in for the *whole* top-end loss.  This one models
 * the inductance explicitly - the coil's impedance rises with
 * frequency, the current falls, and so does the force - which costs
 * about 6dB/octave from the corner on its own.  Applying the old cliff
 * on top of that counted the same rolloff twice and buried the presence
 * peak: measured against Celestion's published Vintage 30 curve, the
 * real driver peaks +8.6dB at 2.5kHz, the old file reached +3.5 and
 * this one reached +0.3.
 *
 * So Modern-4x12 is fitted to that published curve with the model's own
 * inductance already in the path, and scores 1.9dB rms over 150Hz-8kHz.
 * **A big presence boost is not a fudge**: a real driver's breakup
 * resonance does have to beat its own inductance, because a real driver
 * has both.
 *
 * ====================================================================
 * WHAT IS MEASURED AND WHAT IS TASTE
 * ====================================================================
 * Modern-4x12 is a 12" 8 ohm guitar driver whose small-signal constants
 * are published Thiele-Small numbers or derived from them (Bl from Qes,
 * Rms from Qms), and whose voicing is fitted to a digitised curve.
 *
 * **The other four rows are archetypes, not fits.**  Nobody publishes
 * Klippel Bl(x) and K(x) curves for a driver you can buy, and nobody
 * publishes anything at all for "a small combo".  The rows are chosen
 * so that five things a player would recognise as different cabinets
 * come out of one model, and the numbers are picked to put them in the
 * right places relative to each other.  This is physically structured,
 * not physically accurate, and it is not pretending to be a modeller
 * that has heard the actual amplifier.
 */

/* True of drivers in general rather than of any one of them. */
#define CAB_XOFF	0.15f		/* coil sits this far off centre, in xb */
#define CAB_TC_MAX	300.0f		/* where a coil lets go, K */
#define CAB_TAU		1.0f		/* how fast it gets there, seconds */
#define CAB_ALPHA	0.00393f	/* copper temperature coefficient, /K */

/*
 * Volts at the amplifier terminals for a full-scale sample.  28V is
 * about 50W into 8 ohm, which is what the number means; what it does
 * *not* mean is that a full-scale sample here is 50W, because the
 * signal reaching a cab sim in this pedal is a normalised float and its
 * relationship to volts is whatever Drive says it is.  So this is the
 * unit and Drive is the mapping, and Drive is where the arbitrariness
 * lives.
 *
 * Which is why Drive spans 0 to +30dB rather than sitting near unity.
 * The reason the cone needs that much asking is physics rather than a
 * bad constant: excursion falls as 1/f^2 above resonance, and a guitar
 * lives two to three octaves above 75Hz.  A real cone does not move far
 * on a guitar either.
 *
 * **The range is wide because it spans rigs, not settings.**  Measured
 * as the residual against the same model with its large-signal terms at
 * zero - which is the nonlinearity and nothing else, since the
 * small-signal response is identical - on one passage of real guitar:
 *
 *	Drive        through [KLON]      straight in
 *	  0 dB           -21.3 dB          -37.2 dB
 *	 +15                -10.5             -24.4
 *	 +20                 -6.2             -19.0
 *	 +30                 -2.1             -10.7
 *
 * A guitar straight in at the top of the travel lands where a driven
 * one sits at half.  So +30dB is not an extreme setting that nobody
 * wants; it is the weak-pickup end of the same useful span, and the
 * knob has to reach it.
 *
 * The bottom used to go to -10dB and that part was genuinely wasted: it
 * bought 2.5dB of change across a quarter of the travel, none of it
 * audible.  A range being wide is not the same fault as a range being
 * dead, and only the second one is a fault.
 */
#define CAB_VFS		28.0f

#define CAB_DT		(1.0f / SAMPLES_PER_SEC)

/*
 * One speaker.
 *
 * The small-signal five (fs, qms, le, mms, re, bl) are what a
 * datasheet prints.  The large-signal five are where it stops being
 * linear and how nastily - see the box above about which of those are
 * measurements and which are taste.  The voicing at the end is the cone
 * bending and the box, which the integrator knows nothing about.
 *
 * The Axis pot slides between the two ends of pres_lo/pres_hi and of
 * cliff_lo/cliff_hi, so a row says how bright that speaker gets rather
 * than the pot meaning the same hertz on all five.
 */
struct cab_model {
	float fs, qms, le, mms, re, bl;
	float xb, xk, xhard, motor, susp;
	float rth;
	float scoop_f, scoop_db;
	float pres_f, pres_q, pres_lo, pres_hi;
	float cliff_lo, cliff_hi;
};

/*
 * __not_in_flash() because cab_init() runs on the audio core, and a
 * table it reads has to be in RAM for the same reason its code does:
 * saving a scene turns XIP off, and for as long as the erase takes
 * every flash address reads back nonsense.  scripts/check-audio.py
 * fails the build over exactly this, which is the only reason it is not
 * a bug that shows up once a month and never reproduces.
 */
static const struct cab_model __not_in_flash("audio") cab_models[] = {
	/*
	 * A small open-backed combo speaker.  Light cone, small magnet,
	 * so it moves furthest per volt of anything here and runs out of
	 * travel first - about 8dB before Modern-4x12 does.  Narrow at
	 * both ends and no scoop at all, which is what "honky" is.
	 */
	{
		.fs = 110.0f, .qms = 5.0f, .le = 0.70e-3f,
		.mms = 0.012f, .re = 6.0f, .bl = 9.0f,
		.xb = 1.2e-3f, .xk = 1.5e-3f, .xhard = 4.0e-3f,
		.motor = 1.5f, .susp = 1.5f, .rth = 40.0f,
		.scoop_f = 350.0f, .scoop_db = 0.0f,
		.pres_f = 2000.0f, .pres_q = 1.2f,
		.pres_lo = 6.0f, .pres_hi = 18.0f,
		.cliff_lo = 2500.0f, .cliff_hi = 5500.0f,
	},
	/*
	 * Open-backed 1x12, light cone, low inductance.  The brightest
	 * row - its inductive corner is the highest of the five - and the
	 * most scooped.
	 */
	{
		.fs = 95.0f, .qms = 5.0f, .le = 0.70e-3f,
		.mms = 0.020f, .re = 6.4f, .bl = 11.5f,
		.xb = 1.8e-3f, .xk = 2.2e-3f, .xhard = 6.0e-3f,
		.motor = 1.1f, .susp = 0.9f, .rth = 20.0f,
		.scoop_f = 400.0f, .scoop_db = -4.0f,
		.pres_f = 3800.0f, .pres_q = 0.9f,
		.pres_lo = 8.0f, .pres_hi = 20.0f,
		.cliff_lo = 3500.0f, .cliff_hi = 8000.0f,
	},
	/*
	 * Closed-back 4x12 with the older, warmer British 12".  More wire
	 * on the coil than the row below, so a lower corner and a softer
	 * top; less travel before it droops, which is the whole of that
	 * speaker's reputation.
	 */
	{
		.fs = 75.0f, .qms = 4.0f, .le = 1.10e-3f,
		.mms = 0.024f, .re = 6.8f, .bl = 12.5f,
		.xb = 1.5e-3f, .xk = 1.9e-3f, .xhard = 7.0e-3f,
		.motor = 1.3f, .susp = 1.0f, .rth = 15.0f,
		.scoop_f = 400.0f, .scoop_db = -1.0f,
		.pres_f = 2600.0f, .pres_q = 1.1f,
		.pres_lo = 6.0f, .pres_hi = 18.0f,
		.cliff_lo = 2800.0f, .cliff_hi = 6000.0f,
	},
	/*
	 * Closed-back 4x12, the modern high-power 12".  The only row with
	 * a published response behind it, and the default for that
	 * reason: everything else here is placed relative to it.
	 */
	{
		.fs = 75.0f, .qms = 4.5f, .le = 0.90e-3f,
		.mms = 0.027f, .re = 6.6f, .bl = 13.7f,
		.xb = 2.0e-3f, .xk = 2.5e-3f, .xhard = 8.0e-3f,
		.motor = 1.0f, .susp = 1.0f, .rth = 12.0f,
		.scoop_f = 400.0f, .scoop_db = -1.5f,
		.pres_f = 3200.0f, .pres_q = 1.0f,
		.pres_lo = 8.0f, .pres_hi = 20.0f,
		.cliff_lo = 3000.0f, .cliff_hi = 7000.0f,
	},
	/*
	 * A 15" bass driver.  Heavy cone and a big motor, so it moves a
	 * quarter as far per volt as the first row and has four times the
	 * travel to do it in - about 14dB of headroom over Modern-4x12,
	 * which is another way of saying it stays linear on a guitar.
	 * Two and a half millihenries of coil puts the corner below
	 * 350Hz, and there is no breakup peak to speak of.
	 */
	{
		.fs = 45.0f, .qms = 3.0f, .le = 2.50e-3f,
		.mms = 0.075f, .re = 5.4f, .bl = 17.0f,
		.xb = 5.0e-3f, .xk = 6.0e-3f, .xhard = 15.0e-3f,
		.motor = 0.8f, .susp = 1.2f, .rth = 6.0f,
		.scoop_f = 700.0f, .scoop_db = -2.0f,
		.pres_f = 2200.0f, .pres_q = 0.7f,
		.pres_lo = 0.0f, .pres_hi = 6.0f,
		.cliff_lo = 2000.0f, .cliff_hi = 5000.0f,
	},
};

struct {
	float x, v, i;		/* position m, velocity m/s, current A */
	float tc;		/* voice coil temperature rise, K */

	float inv_mms, inv_le, inv_xb, inv_xk;
	float k0, rms, rout, re0, bl0, xoff, xhard;
	float motor, susp, rth;
	float drive;		/* sample -> volts */
	float norm;		/* m/s^2 -> sample */
	float rmax;		/* stability clamp on Re+Rout */
	float thermal;		/* dt/tau */

	/* Not the model - the cone bending, and the box.  See above. */
	struct biquad scoop, presence, hi_cut_1, hi_cut_2;
} cab;

static inline void cab_init(unsigned char pot[10])
{
	/*
	 * Bounded because this indexes a table.  A pot byte is supposed
	 * to have been checked against the enumeration before it got
	 * here, and a value that was not is a five-element array read at
	 * 120 - which is not the kind of bug worth trusting a caller
	 * with.
	 */
	int which = pot[CAB_CABINET];
	if (which < 0 || which >= (int) ARRAY_SIZE(cab_models))
		which = 0;

	const struct cab_model *m = &cab_models[which];
	float w0 = 6.2831853f * m->fs;
	float axis = cab_axis_pot(pot);

	cab.rout = cab_resonance_pot(pot);
	cab.re0 = m->re;
	cab.bl0 = m->bl;
	cab.motor = m->motor;
	cab.susp = m->susp;
	cab.rth = m->rth;
	cab.xhard = m->xhard;
	cab.xoff = CAB_XOFF * m->xb;

	cab.inv_mms = 1.0f / m->mms;
	cab.inv_le = 1.0f / m->le;
	cab.inv_xb = 1.0f / m->xb;
	cab.inv_xk = 1.0f / m->xk;

	/*
	 * A row names its resonance and its mechanical Q, because those
	 * are what a datasheet prints; the spring and the damping follow.
	 */
	cab.k0 = m->mms * w0 * w0;
	cab.rms = w0 * m->mms / m->qms;

	cab.drive = CAB_VFS * db_to_level(cab_drive_pot(pot));

	/*
	 * Above resonance and below the inductive corner the back-EMF is
	 * negligible and the mass dominates, so the small-signal
	 * acceleration is Bl*e/(Mms*(Re+Rout)).  Dividing that back out
	 * is what makes Drive a control over how hard the cone is worked
	 * rather than a volume knob: the level only moves once the
	 * large-signal shapes start to bite, which is the whole point.
	 *
	 * It also puts every row at the same small-signal level, so
	 * switching cabinet is a change of tone and of how early it lets
	 * go, and not a change of volume.
	 */
	cab.norm = m->mms * (m->re + cab.rout) / (m->bl * cab.drive);

	/*
	 * Explicit Euler on the electrical equation is stable while
	 * dt*(Re+Rout)/Le < 2, so this clamp is a property of the row's
	 * inductance and has to be computed from it.  It also puts a
	 * floor under what a row may declare: the worst case is Resonance
	 * at 24 ohm into a coil at CAB_TC_MAX, which is about 38 ohm, so
	 * a row with much under 0.6mH would have this clamp fire in
	 * ordinary use - and a clamp that fires does not blow up, it
	 * quietly stops obeying the knob, which is worse.  The lowest
	 * here is 0.70mH, giving 53.8 against 37.1.
	 *
	 * The clamp is here rather than in a comment because this is the
	 * one bound in the file a parameter can walk into, and because
	 * -ffast-math means an isfinite() further downstream is something
	 * the compiler is entitled to delete.  Everything here is guarded
	 * by clamping values, never by testing for NaN.
	 */
	cab.rmax = 1.6f * m->le * SAMPLES_PER_SEC;

	cab.thermal = CAB_DT / CAB_TAU;

	/*
	 * The cone bending.  Axis slides the presence peak and the cliff
	 * together because a real driver does not separate them, and each
	 * row says how far its own travel goes.
	 */
	biquad_peaking(&cab.scoop, m->scoop_f, 1.0f, db_to_A(m->scoop_db));
	biquad_peaking(&cab.presence, m->pres_f, m->pres_q,
		       db_to_A(linear(axis, m->pres_lo, m->pres_hi)));

	float hicut = linear(axis, m->cliff_lo, m->cliff_hi);
	biquad_lpf(&cab.hi_cut_1, hicut, 0.7f);
	biquad_lpf(&cab.hi_cut_2, hicut, 0.7f);

	/*
	 * x, v, i and tc are deliberately not reset.  init() runs on
	 * every pot change, and zeroing a cone that is mid-swing is a
	 * click.  They start at zero because the struct does.
	 *
	 * Changing Cabinet *will* click, because it changes the mass and
	 * the spring underneath a cone that is already moving.  That is
	 * accepted rather than overlooked: nobody swaps speakers in the
	 * middle of a phrase, and the alternative is a fade that would
	 * have to live on the audio core to do any good.
	 */
}

static inline float cab_step(float in)
{
	float e = in * cab.drive;

	/* A hot coil is a more resistive coil, so this comes first. */
	float re = cab.re0 * (1.0f + CAB_ALPHA * cab.tc);
	float r = clamp(re + cab.rout, 1.0f, cab.rmax);

	/*
	 * The two large-signal shapes.  Both are even in x - a symmetric
	 * driver makes odd harmonics only - so the coil is placed
	 * slightly off centre, which is both true of real drivers and
	 * where the even harmonics come from.
	 *
	 * Bl droops as the coil leaves the gap.  Written as a quotient
	 * rather than a polynomial because it can then never go negative
	 * or run away: less Bl means less force means less excursion, so
	 * the nonlinearity limits itself.
	 */
	float xr = (cab.x - cab.xoff) * cab.inv_xb;
	float xr2 = xr * xr;
	float bl = cab.bl0 / (1.0f + cab.motor * xr2);

	/* The surround stiffens instead, which is the opposite sign. */
	float xk = cab.x * cab.inv_xk;
	float k = cab.k0 * (1.0f + cab.susp * xk * xk);

	cab.i += CAB_DT * (e - cab.i * r - bl * cab.v) * cab.inv_le;

	float a = (bl * cab.i - cab.rms * cab.v - k * cab.x) * cab.inv_mms;

	/*
	 * Symplectic Euler: v moves on the old x, then x moves on the
	 * *new* v.  One line's difference from the obvious thing and it
	 * is the difference between an oscillator that keeps its energy
	 * and one that gains a little every cycle.  Stable while
	 * w0*dt < 2; the stiffest row at the far end of its surround is
	 * about 0.05, so there are more than an order of magnitude.
	 */
	cab.v += CAB_DT * a;
	cab.x += CAB_DT * cab.v;

	/*
	 * The backstop, and it is not unreachable.  Measured on a real
	 * guitar into Modern-4x12: a hot signal (through [KLON]) starts
	 * hitting it at about Drive 24dB and hits it 203 times in fifteen
	 * seconds at Drive 30, while the same guitar with no drive in
	 * front peaks at 6mm and never reaches it at any setting.
	 *
	 * That is worth knowing rather than fixing, because it is where
	 * Drive stops sounding good: below it the motor droop rolls off
	 * smoothly and above it the cone is slamming into a stop, which
	 * is a real thing a driven speaker does and a nastier one.  The
	 * velocity is killed rather than reflected, so it is cruder than
	 * a real collision; a restitution term would be one line if it
	 * ever seems worth it.
	 */
	if (cab.x > cab.xhard || cab.x < -cab.xhard) {
		cab.x = clamp(cab.x, -cab.xhard, cab.xhard);
		cab.v = 0.0f;
	}

	/* Into breakup, which is the thing worth showing on the LED. */
	if (xr2 > 1.0f)
		cab_effect.intense = 1;

	/*
	 * Power into the coil, in seconds rather than milliseconds.  The
	 * clamp is at the temperature a coil comes apart at, and it also
	 * keeps 'r' above inside the bound rmax was derived for.
	 *
	 * How much of this a row gets is its own business: a small cone
	 * with no air around it cooks, and a 4x12 spreads the same watts
	 * over four of them.  Which is why this is not a pot - it is a
	 * fact about the speaker and not about the player.
	 */
	cab.tc += cab.thermal *
		  (cab.i * cab.i * re * cab.rth - cab.tc);
	cab.tc = clamp(cab.tc, 0.0f, CAB_TC_MAX);

	/* Radiated pressure follows cone acceleration, not position. */
	float out = a * cab.norm;

	/* And then the cone bends, which is everything above about 1kHz. */
	out = biquad_step(&cab.scoop, out);
	out = biquad_step(&cab.presence, out);
	out = biquad_step(&cab.hi_cut_1, out);
	out = biquad_step(&cab.hi_cut_2, out);
	return out;
}
