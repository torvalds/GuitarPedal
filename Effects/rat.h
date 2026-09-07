// NAME: Rat Sketch [RAT]
// PRIORITY: 57
// POT: "Distortion" LINEAR(0 1) = 0.45
// INFO: How much resistance is in the op-amp's feedback, which is all
// INFO: this pedal's gain is. Fully down is nearly a clean boost; fully
// INFO: up is 67 dB and the op-amp spends most of its time on a rail.
// POT: "Filter" LINEAR(0 1) = 0.50
// INFO: Backwards, like the pedal it comes from: turning it up makes it
// INFO: darker. One pole, sliding from 32 kHz down to 475 Hz.
// POT: "Sweep" LINEAR(0 1) = 0.00
// INFO: Not on a real RAT. It puts up to a kilohm under the gain
// INFO: network, which takes the treble emphasis away and the gain with
// INFO: it - fully down is the stock pedal.
// POT: "Mode" ENUM(Silicon Stacked LED) = 0
// INFO: What the clamp is made of: one silicon diode each way, two in
// INFO: series, or a red LED. A diode has no clipping level - it is a
// INFO: logarithm and it keeps rising - so these are not three heights
// INFO: so much as three places to start leaning. Around 0.63, 1.26 and
// INFO: 1.81 volts at a milliamp, and all three still climbing above it.
// POT: "Volume" LINEAR(0 1) = 0.50
// INFO: An audio taper like the pedal's, so noon is about
// INFO: 18 dB down rather than 6. This pedal is loud.

/*
 * The ProCo RAT.
 *
 * One op-amp with a great deal of gain, a diode clamp on its output, one
 * pole of treble cut and a buffer.  There are no transistors in the
 * signal path, so the whole of it is a feedback network, two diodes and
 * a filter - which is why it is here beside [PI]: it is the distortion
 * with the least in it, and so the one where what is left is the
 * argument.
 *
 * Every value comes from Validation/spice/rat-helios.cir, which is the
 * Aion FX Helios - a documented clone, and the one on the bench.  So of
 * the three circuit emulations here this is the one that can be argued
 * with by a real pedal rather than only by ngspice, and most of what is
 * written down below was settled that way.
 *
 * THE GAIN NETWORK IS INSIDE THE LOOP, WHICH IS WHERE THE CIRCUIT HAS IT
 *
 * A RAT's feedback leg is two series RC branches in parallel and the
 * Distortion rheostat is the top of the divider, so the gain is
 *
 *	1 + Zf * (s*C5/(1 + s*Ra*C5) + s*C6/(1 + s*R4*C6))
 *
 * and for a long time this file computed exactly that and applied it,
 * which is three one-pole filters and an addition.  There is nothing
 * approximate about the arithmetic: 4.7uF into 560R lifts everything
 * above 60 Hz, 2.2uF into 47R lifts it again above 1.5 kHz, and the
 * result is several decibels an octave of pre-emphasis that the Filter
 * knob then takes back out.  That shape is the pedal.
 *
 * Where that network sits is the whole design of this file.  A gain
 * worked out as a transfer function and then clipped goes on shaping
 * the signal through the part of the cycle when the real amplifier has
 * stopped - and worse than the level error, it cannot move a zero
 * crossing, because a linear filter followed by a monotone memoryless
 * map never does.  Its duty cycle would be exactly 50% at every
 * setting.  The circuit's is 45.9%, and for a clipped wave the duty
 * cycle *is* the even harmonics: H2/H1 is |cos(pi*d)|, which is nothing
 * at all at a half.  This pedal's second harmonic sits 20 dB down, and
 * no amount of work on the clamp puts it there.
 *
 * So the network is driven from the railed output, as
 * conductances at the feedback node, and the amplifier is one
 * integrator inside it - see rat_opamp().  When it saturates the loop
 * opens, C5 and C6 go on charging from wherever the node was, and when
 * it comes back out is decided by that charge and by which rail it had
 * been sitting against.  Two unequal rails and a capacitive feedback leg
 * make a duty cycle; two unequal rails on their own only make an
 * amplitude asymmetry, and the charge balance on C7 downstream takes
 * that straight back out again.
 *
 * WHAT IS SIMPLIFIED, WHICH IS NOW A SHORTER LIST
 *
 * The op-amp is a single pole.  A real OP07 has more than one, and it
 * has a slew rate - which is measured as not mattering here rather than
 * assumed away: the closed-loop corner is 2 kHz at Distortion noon and
 * 560 Hz at the top, so a rail-to-rail swing through it has a maximum
 * slope of 0.07 and 0.02 V/us against the part's 0.3.  The bandwidth
 * limit wins by four to fifteen times everywhere.
 *
 * The JFET follower and the output network are one number, RAT_KOUT.
 * It is flat to a couple of tenths across the band, so this is a good
 * approximation of a stage the schematic cannot settle anyway - it draws
 * a 2N5457 and the kit ships a J112 or a PN4303, which bias half a volt
 * apart.
 *
 * Everything runs at the sample rate, so the diode clamp aliases.  What
 * that costs has not been measured on this effect.
 *
 * And the clamp's series resistance is a lump.  31 ohms was fitted to
 * the pedal, and a 1N914's own bulk is single digits, so most of it is
 * the Mode switch or the follower sagging - see the constants below,
 * where the choice that follows from not knowing is written down.
 *
 * WHAT THAT LEAVES, AGAINST THE DECK
 *
 * Small-signal shape from 40 Hz to 5 kHz, worst error: 0.36 dB at
 * Distortion noon and 1.17 dB at full.  Duty cycle 46.27% against the
 * pedal's 46.83%, second harmonic -18.6 dB against -19.6.
 *
 * And a plateau's droop 1.32 dB against 1.40, once the bench's own
 * input high-pass is taken back out of the capture - see
 * Validation/loop.py, which is where that correction and the reason it
 * has to exist are written down.
 */

#define RAT_VPEAK	1.41421356f

/* The gain network, in the units the schematic gives them in. */
#define RAT_C4		100e-12f
#define RAT_C5		2.2e-6f
#define RAT_C6		4.7e-6f
#define RAT_C8		3.3e-9f
#define RAT_R3		47.0f		/* under the Sweep pot */
#define RAT_R4		560.0f
#define RAT_R6		1500.0f		/* in front of the Filter cap */
#define RAT_RDIST	100000.0f
#define RAT_RSWEEP	1000.0f
#define RAT_RFILT	100000.0f

/*
 * All three 100kA pots are audio taper, and a cube is close enough to
 * one: half the rotation is an eighth of the track, where a real A curve
 * is about a tenth.  It matters more than a taper usually does here,
 * because resistance goes into a gain as a logarithm - a linear
 * Distortion knob would do forty of its sixty decibels in the first
 * tenth of its travel and nothing much afterwards.
 *
 * Volume had it missing and was a plain scalar, which put this model
 * 10.6 dB louder than the pedal with every knob at noon.
 *
 * Measured on the Helios itself, by taking the loop's small-signal gain
 * with Level at 100% and again at noon and subtracting: the pot at noon
 * is -15.68 dB, and flat to 0.23 dB from 60 Hz to 14 kHz, so it is a
 * clean divider and nothing else.  That is 16.4% of the track.  A linear
 * law says -6.02, a square -12.04 and this cube -18.06.
 *
 * The two 100kA pots do not agree with each other, and that is the
 * reason for leaving one shared cube rather than fitting each to its own
 * single measured point: working back through the netlist, Distortion at
 * noon implies about 10.9% of its track where Level sits at 16.4%.  Both
 * are ordinary A-taper numbers and the parts are nominally the same, so
 * it is spread and not a law waiting to be discovered.  The cube lands
 * 1.1 dB high on gain and 2.4 dB low on level, which partly cancel: with
 * both knobs at noon this model reads +12.40 dB against the pedal's
 * +13.85.
 */
#define RAT_TAPER(p)	((p) * (p) * (p))

/*
 * The input and output coupling.  The one in front of the diodes is not
 * here, because it is not a corner: see RAT_C7 below.
 */
#define RAT_F_IN	7.2f
#define RAT_F_OUT	7.2f

/*
 * C7 and R5, kept apart rather than multiplied into a 33.9 Hz corner.
 *
 * A high-pass would be the right way to write this if the node behind
 * R5 had a resistor to ground.  It does not.  R5 leads back to C7, the
 * diodes are the only other thing on the node, and the Filter pot leads
 * to C8 and C9, which are capacitors - so there is no DC path to ground
 * anywhere except through the diodes themselves.
 *
 * That makes the charge on C7 a state and not a filter, and the
 * difference is the whole of this effect's second harmonic.  A
 * high-pass forces its output to mean zero whatever went in; charge
 * balance lets the operating point sit wherever the two half cycles
 * pass equal charge, which for a drive that meets one rail harder than
 * the other is not zero.  An offset moves the zero crossings, the zero
 * crossings are the duty cycle, and for a clipped wave the duty cycle
 * is exactly where the even harmonics come from: H2/H1 is |cos(pi*d)|,
 * which is nothing at all at d = 50%.
 */
#define RAT_C7		4.7e-6f
#define RAT_R5		1000.0f

/*
 * The supply, and where the op-amp's output stops short of each end.
 *
 * Measured at the pedal's own pins rather than worked out: 9.09 V on the
 * op-amp's V+, and the bias divider puts the quiescent output at 4.549 V,
 * read at pin 6 rather than at the + input because the divider is 50k of
 * source impedance and a meter takes 400 mV out of the answer there.
 *
 * The swing itself is off a scope on pin 6 at Distortion full, which is
 * the only direct look this file has ever had at it: the output sits at
 * 8.2 V and 1.56 V, so 0.89 V short of the supply and 1.56 V short of
 * ground.  The two ends are not the same distance away, which is the
 * wrong way round for a bipolar output stage and is what the OP07's own
 * curves say anyway.
 *
 * Measured rather than fitted, and that is a deliberate trade.  These
 * two could be tuned through the loop against the pedal's output three
 * stages downstream, and doing so would land closer on paper - but a
 * constant tuned until two mistakes cancel is worth less than a
 * measured one that leaves the remaining error where it can be found.
 * What this model still gets wrong is attributable instead of hidden.
 *
 * How well pinned is it?  The scope's own Vtop/Vbase read 8.188 and
 * 1.561 and a cursor on the zoomed trace read 8.213 and 1.565, so about
 * a tenth of a volt either way.  That is not nothing here: nulling the
 * two readings against each other puts a tenth of a volt on the upper
 * rail at 0.29% of duty cycle and 0.65 dB of H2, the same to within a
 * hundredth across all three Mode positions and both drives.  The
 * gap left to the pedal is 0.8% and 1.7 dB - bigger than the reading's
 * own uncertainty, but the same order, so it is worth knowing that
 * chasing the last decibel of H2 from this constant would be chasing
 * the measurement rather than the circuit.
 *
 * The plateaus themselves are flat, top and bottom, which is worth
 * recording because it was a live question: the droop this pedal puts on
 * a clipped note is made downstream of here, not by the amplifier.
 *
 * That asymmetry is not a detail.  It is where every even harmonic this
 * pedal makes comes from.  A single symmetric limit produces none of
 * them, and an asymmetric one outside the loop produces an amplitude
 * asymmetry that the clamp's coupling then removes; it only becomes a
 * duty cycle from inside the loop, which is where rat_opamp() applies
 * it.
 *
 * The two are written signed, because a rail is a place and not a
 * distance: that is what lets the limit be clamp(o, DN, UP) and read
 * like what it is.
 *
 * AND HARD, WHICH IS NOT AN APPROXIMATION HERE
 *
 * A real output stage runs out of swing gradually rather than at a
 * corner, so rounding the last tenth of a volt looks like the honest
 * thing to do.  It is not worth doing, and the reason is downstream:
 * what these rails feed is a diode clamp that is limiting at 0.61 V.
 * Everything within half a volt of a 3 V rail arrives at the same place
 * one line later, so the shape of the approach is spent on a signal
 * that is about to be flattened anyway.
 *
 * What the shape could still reach is *when* saturation ends, since
 * that is decided at the rail rather than after it.  That is the
 * measurement to make if this is ever revisited: a soft knee changes
 * the duty cycle or it does not, and the duty cycle is the only thing
 * in this file the last tenth of a volt could plausibly move.
 */
#define RAT_VSUP	9.09f
#define RAT_SWING_HI	0.89f
#define RAT_SWING_LO	1.56f
#define RAT_RAIL_UP	(0.5f * RAT_VSUP - RAT_SWING_HI)
#define RAT_RAIL_DN	(RAT_SWING_LO - 0.5f * RAT_VSUP)

/*
 * The op-amp's gain-bandwidth, which is the other thing an ideal one
 * does not have.
 *
 * An OP07 is a precision part and a slow one.  460 kHz, fitted to the
 * pedal through the loop and inside the datasheet's 0.4 to 0.6 MHz.
 * Against a gain network asking for forty-odd decibels that is a corner
 * in the audio band, not above it - and without it this model is 7.9 dB
 * bright at 5 kHz with the Distortion at noon, and 22.8 dB at the top
 * of its travel.
 */
#define RAT_GBW		460000.0f

/*
 * The three clamps, as the diodes they are rather than as a shape.
 *
 * A clipping level and a knee would be the cheap way to write this, and
 * it costs more than it looks like it does.  A diode has no clipping
 * level: its drop is the logarithm of its current, so the node keeps climbing
 * about a hundred millivolts a decade for as long as the drive keeps
 * rising.  Measured on the pedal at unity gain, the last decibel before
 * full scale still moves the output 0.21 dB.  A parabola that goes
 * exactly flat above its knee cannot do that, and what it costs is not
 * subtle: the flat top of a clipped note stops tilting as the note
 * decays, and with nothing tilting there is nothing for the charge on
 * C7 to show through, so the duty cycle sits at 50% and the second
 * harmonic goes with it.
 *
 * The silicon numbers are the pedal's own, measured at Distortion
 * minimum where the op-amp's gain is 1 and the input drives the diodes
 * through R5 alone - which turns a level ladder into a transfer curve.
 * Fitting V = N*Vt*ln(I/Is) + I*Rs over 3 uA to 773 uA:
 *
 *	N   1.859        Is   2.03 nA        Rs   31 ohm
 *
 * 0.62 mV rms of residual over a 289 mV span, against 4.1 mV for the
 * same fit without the Rs term, whose absence shows up as an apparent
 * slope rising from 112 mV/decade in the bottom half to 137 in the top.
 * That is an IR drop and not an ideality.
 *
 * WHERE THE 31 OHMS LIVES IS NOT SETTLED, AND IT DECIDES ONE LINE BELOW
 *
 * A 1N914's own bulk resistance is single-digit ohms, so most of this is
 * something else in series with the measurement: the Mode switch's
 * contacts, or the JFET follower sagging slightly at large signal.  Two
 * of those three are outside the diode and one is inside it, and only
 * the inside one should be multiplied when the Stacked position puts
 * two junctions in series.  So it is not multiplied.  That is a choice
 * recorded rather than a measurement, and separating them is the
 * experiment that would settle it.
 *
 * The LED is the netlist's part and not a measurement, because it
 * cannot be measured the way the other two were: it clamps at 1.81 V,
 * above the 1.414 V the loop can put into the pedal, and putting gain
 * in front to reach it brings the op-amp's rails into the same octave -
 * so what would come back is two mechanisms and not this one.
 *
 * The two red LEDs are across the node in every position, not only this
 * one.  They contribute nothing to the others and are left out of them:
 * where Stacked has 4.3 mA through its silicon the LEDs pass 0.24 uA,
 * four decades down, and in Silicon the node never gets near them.
 */
#define RAT_D_N		1.859f		/* one silicon junction */
#define RAT_D_IS	2.03e-9f
#define RAT_D_RS	31.0f
#define RAT_LED_N	1.9f		/* LEDRED, from the netlist */
#define RAT_LED_IS	1e-19f
#define RAT_LED_RS	3.0f

/*
 * kT/q at 27 C, and the current the diode law is written around.
 *
 * The law wants a reference somewhere near where the diode actually
 * works rather than at Is, which is nanoamps: exp() of the distance
 * from a milliamp stays inside a couple of dozen e-folds, where the
 * distance from Is is fifty of them for an LED and past what pow2()
 * covers.  Same equation, arithmetic that fits in a float.
 */
#define RAT_VT		0.025852f
#define RAT_IREF	1.0e-3f

/*
 * The node, solved at build time.
 *
 * scripts/rat_clamp.py takes the constants above, solves
 * drive = v + I(v)*(Rs + R5) by bisection at every point of a table,
 * and emits one table per Mode.  See that file for why a uniform table
 * is enough for a curve that looks like it has a corner in it.
 *
 * Solving it here rather than per sample is the whole reason it is a
 * table.  The equation has no closed form, so a runtime solve means an
 * upper bound and a handful of Newton steps - several transcendentals a
 * sample, for a curve that depends on nothing but the Mode switch and
 * cannot move while a note is playing.  Spending them once per build
 * instead buys an index and a lerp, and buys accuracy with it: the
 * table can afford to be converged where a per-sample solver has to
 * stop early.
 */
#include "rat_clamp.h"


/*
 * What the follower and the output network do to the level, which is
 * -1.95 dB and is a measurement rather than an arithmetic: a JFET
 * follower's gain depends on where it biased, and where it biased
 * depends on which of the three parts the kit shipped.  Flat across the
 * band to a couple of tenths of a decibel, so it is a number.
 */
#define RAT_KOUT	0.798f


static struct {
	float kout;			/* follower, volume and back to samples */

	/*
	 * The op-amp and its feedback network, as a loop rather than as
	 * a gain.  lp_c is how much of the output the feedback node can
	 * see *this sample* - the capacitors are states and hold their
	 * voltage across it, so the instantaneous divider is resistive.
	 * lp_den is the implicit solve; see rat_opamp().
	 */
	float lp_a, lp_1ma_over_c, lp_c;
	float lp_ga, lp_gb, lp_c4t, lp_inv_s;
	float lp_a5, lp_a6;
	float o, vc5, vc6, ufb;

	/*
	 * The clamp, as the four numbers the diode law needs plus the
	 * three reciprocals the solver would otherwise divide by every
	 * sample.  d_v1 is where this stack sits at RAT_IREF, which is
	 * the only place the law is written from.
	 */
	const float *clamp_curve;	/* which Mode's table */
	float d_rs, d_inv_rtot;

	struct single_pole_state hp_in, lp_filt, hp_out;
	struct single_pole_coeff hp_in_c, lp_filt_c, hp_out_c;
	float vc7;			/* what the diodes have put on C7 */
} rat;

void rat_init(unsigned char pot[10])
{
	float dist = RAT_TAPER(rat_distortion_pot(pot));
	float filt = RAT_TAPER(rat_filter_pot(pot));
	float rd = dist * RAT_RDIST;
	float ra = RAT_R3 + rat_sweep_pot(pot) * RAT_RSWEEP;
	int mode = (int)rat_mode_pot(pot);

	/*
	 * A rheostat that reaches zero is a divide by zero here and a
	 * short in the circuit, and neither is what the end of a track
	 * is: a wiper has contact resistance.  A tenth of an ohm is
	 * nothing against the 47 it is in series with.
	 */
	if (rd < 0.1f)
		rd = 0.1f;

	rat.kout = RAT_KOUT * RAT_TAPER(rat_volume_pot(pot)) / RAT_VPEAK;

	rat.lp_filt_c =
		single_pole_freq(1.0f / (TWOPI * (filt * RAT_RFILT + RAT_R6)
					 * RAT_C8));
	/*
	 * The loop, as conductances at the feedback node.
	 *
	 * Nothing here solves for a corner any more.  The old model
	 * multiplied out the network's gain, applied it, and then put a
	 * separate pole after it at GBW divided by that gain - which
	 * meant iterating to find the frequency where those two agreed,
	 * because the Distortion knob moves it across most of the band.
	 * A loop does not need to be told: an integrator inside a
	 * feedback path settles at GBW/|A_cl| by itself, at every
	 * frequency at once and without the single-pole approximation
	 * the old solve had to make.
	 *
	 * gd is the feedback element, R and C together, because C4 is
	 * 100 pF and at 48 kHz that is 4.8 uS against the rheostat's
	 * 10 uS at full Distortion - the same order, not a rounding.
	 */
	{
		float gd = 1.0f / rd + RAT_C4 * SAMPLES_PER_SEC;
		float ga = 1.0f / ra;
		float gb = 1.0f / RAT_R4;

		rat.lp_c4t = RAT_C4 * SAMPLES_PER_SEC;
		rat.lp_ga = ga;
		rat.lp_gb = gb;
		rat.lp_inv_s = 1.0f / (gd + ga + gb);
		rat.lp_c = gd * rat.lp_inv_s;
		rat.lp_a = expf(-TWOPI * RAT_GBW / SAMPLES_PER_SEC
				* rat.lp_c);
		rat.lp_1ma_over_c = (1.0f - rat.lp_a) / rat.lp_c;

		/*
		 * Each branch capacitor charges through its own resistor
		 * towards whatever the node is holding.  Stepped exactly
		 * rather than by Euler: 47 ohms into 2.2 uF is a 1.5 kHz
		 * corner, which is a third of the way to Nyquist and not
		 * somewhere a difference quotient is honest.
		 */
		rat.lp_a5 = 1.0f - expf(-1.0f / (ra * RAT_C5
						 * SAMPLES_PER_SEC));
		rat.lp_a6 = 1.0f - expf(-1.0f / (RAT_R4 * RAT_C6
						 * SAMPLES_PER_SEC));

		rat.o = rat.vc5 = rat.vc6 = rat.ufb = 0.0f;
	}

	rat.hp_in_c = single_pole_freq(RAT_F_IN);
	rat.hp_out_c = single_pole_freq(RAT_F_OUT);

	/*
	 * The clamp is a table per Mode, so this is a pointer and the
	 * series resistance that sits behind it.  Stacked shares the
	 * silicon constants - two junctions in series cost twice the
	 * volts per e-fold and nothing else changes - which is why one
	 * fit covers two of the three positions.
	 */
	if (mode >= 2) {
		rat.clamp_curve = rat_clamp_led;
		rat.d_rs = RAT_LED_RS;
	} else if (mode == 1) {
		rat.clamp_curve = rat_clamp_stack;
		rat.d_rs = RAT_D_RS;
	} else {
		rat.clamp_curve = rat_clamp_si;
		rat.d_rs = RAT_D_RS;
	}
	rat.d_inv_rtot = 1.0f / (rat.d_rs + RAT_R5);

	rat.vc7 = 0.0f;
}

/*
 * The op-amp, with its feedback network round it.
 *
 * The cheap way to write this is three filters and a multiply: work the
 * network's gain out as a transfer function, apply it to the input, and
 * clip the rails onto the answer afterwards.  Every level that comes
 * out of that is right.  The timing is not, and the timing is what a
 * saturating amplifier is *for*.
 *
 * A memoryless clip of a linear filter's output cannot move a zero
 * crossing.  Feed that arrangement a sine and the signal arriving at
 * the diodes crosses zero exactly when the sine does, at every setting,
 * so its duty cycle is 50% by construction and its second harmonic is
 * whatever the clamp alone can make - which is nothing.  The circuit is
 * at 45.9% and its second harmonic is 20 dB down.
 *
 * The difference is that the real rails are *inside* the loop.  When
 * this op-amp saturates the loop opens, C5 and C6 go on charging from
 * wherever the node was, and when the amplifier comes back out is
 * decided by that charge and by which rail it was sitting on.  Two
 * unequal rails and a capacitive feedback leg make a duty cycle; two
 * unequal rails on their own only make an amplitude asymmetry, and the
 * charge balance on C7 downstream takes that straight back out again.
 *
 * THE LOOP IS CLOSED ON THE LAST SAMPLE, AND THE POLE IS TAKEN EXACTLY
 *
 * Everything the feedback network is holding - both branch capacitors
 * and the charge on C4 - comes from the previous sample.  Nothing here
 * iterates and nothing solves a circuit; 'd' is those states referred
 * to the node, and it is known before the amplifier is asked anything.
 *
 * What is not done by a difference quotient is the amplifier's own
 * pole.  The op-amp is one integrator, o' = wu*(x - n) with n = c*o + d,
 * so the linear part of the loop is a single pole at wu*c and it is
 * stepped the way every other pole in this tree is stepped: exactly,
 * with a coefficient worked out once at init.  That is not extra
 * machinery.  It is three multiplies, the same as the Euler form, and
 * the Euler form does not work here.
 *
 * Measured, because it is the kind of claim that should not be taken on
 * trust.  wu/Fs is 60 at this sample rate - a 460 kHz part against a
 * 48 kHz clock - and an explicit step is stable only while k*c < 2.
 * At the top of the Distortion travel c is 6.4e-4 and that is 0.04, so
 * Euler is fine and agrees to a decimal.  At the bottom c approaches 1,
 * because the amplifier is a follower there, and k*c is 60.  Stepping
 * that explicitly does not merely lose accuracy:
 *
 *	Distortion pot   0.00    0.10    0.20    0.45    1.00
 *	k*c             60.08   34.11    3.11    0.30    0.04
 *	fundamental      -272    -272   -43.8   -8.88   -8.74  dB
 *	off the grid      0.0     0.0    -0.0   -18.6   -18.2  dB
 *
 * The bottom fifth of the knob comes out as noise: the loop oscillates
 * at Nyquist, the rails clamp it, and every bit of the output energy is
 * off the harmonic grid.  It does not diverge, which is worse - it
 * sounds like a broken pedal rather than looking like a broken model.
 *
 * There is no tuning that fixes it, because it is not a tuning problem.
 * The closed-loop bandwidth at Distortion minimum really is 460 kHz,
 * and no explicit step holds a pole an order of magnitude past Nyquist.
 * The exact coefficient does, by putting it at Nyquist and staying
 * there.
 *
 * The rail clamps the state itself, which is both the anti-windup and
 * the physics: a real output stage held against a rail has no charge
 * left to unwind, and the feedback network sees the rail rather than
 * where the amplifier would have liked to go.
 */
static inline float rat_opamp(float x)
{
	float d, o, n;

	/* what the capacitors are holding, referred to the node */
	d = (rat.vc5 * rat.lp_ga + rat.vc6 * rat.lp_gb
	     - rat.ufb * rat.lp_c4t) * rat.lp_inv_s;

	o = rat.lp_a * rat.o + rat.lp_1ma_over_c * (x - d);
	o = clamp(o, RAT_RAIL_DN, RAT_RAIL_UP);
	rat.o = o;

	n = rat.lp_c * o + d;
	rat.vc5 += rat.lp_a5 * (n - rat.vc5);
	rat.vc6 += rat.lp_a6 * (n - rat.vc6);
	rat.ufb = o - n;

	return o;
}

/*
 * The clipping node: C7 into R5, the diodes across it, and no third
 * thing.
 *
 * One equation.  The drive that reaches the node is the op-amp's output
 * less whatever charge the diodes have already put on C7; that drive
 * divides between R5 and the diodes, and the diodes' share is set by
 *
 *	I(v) = IREF*exp((v - v1)/vt) - Is
 *
 * where v is across the junctions and vt is what an e-fold of current
 * costs them.  Kirchhoff closes it:
 *
 *	drive = v + I(v)*(Rs + R5)
 *
 * which has no closed form, so it is solved.  Newton, from the previous
 * sample's answer, four steps - see RAT_SOLVE_STEPS.
 *
 * THE UPPER BOUND IS NOT A SAFETY NET, IT IS WHAT MAKES IT CONVERGE
 *
 * Newton on a diode is famously bad in one direction: started below the
 * knee it steps to somewhere the exponential is enormous, and then
 * crawls back down one vt at a time.  Two facts bound the answer from
 * above and both are free.  No more current can flow than the drive
 * would push through the resistors with the diodes shorted, so v can be
 * no more than the voltage that current costs; and v can be no more
 * than the drive itself.  Clamping every iterate to the smaller of
 * those turns the overshoot into the *good* direction - a convex
 * function approached from above converges monotonically - and it is
 * the difference between four steps and forty.
 *
 * The current is then read back off the resistor rather than out of the
 * exponential.  Once v is known, I is exactly (drive - v)/(Rs + R5),
 * which saves the last exp() and is the better answer anyway: it makes
 * the current agree with the resistor to the last bit, so whatever
 * error is left in v lands on the diode's I-V curve rather than on the
 * charge going into C7.  Nothing here is allowed to invent charge.
 */
static inline float rat_diode_clip(float drive)
{
	float m, u, v, i;
	float sign = 1.0f;
	int k;

	m = drive - rat.vc7;
	if (m < 0.0f) {
		sign = -1.0f;
		m = -m;
	}
	/*
	 * Past the end of the table the last segment is extended rather
	 * than the drive clamped, and that is not a detail: the drive
	 * does leave 0..4 V on real playing, because C7's offset swings
	 * further under a picked transient than it does under a steady
	 * tone.  Clamping there would flatten the top of exactly the
	 * transients this pedal is played for, and no table size would
	 * fix it, because the error is not resolution.  Extending is
	 * nearly free to be right about - up there the curve is a
	 * logarithm, so the chord this walks along has a slope of a few
	 * hundred microvolts per volt.
	 */
	u = m * RAT_CLAMP_STEP;
	k = (int)u;
	if (k >= RAT_CLAMP_N)
		k = RAT_CLAMP_N - 1;
	v = linear(u - k, rat.clamp_curve[k], rat.clamp_curve[k + 1]);

	/*
	 * The current is read off the resistor rather than out of the
	 * table, which is both cheaper and better: it agrees with R5 to
	 * the last bit, so whatever interpolation error is left in v
	 * lands on the diode's I-V curve rather than on the charge going
	 * into C7.  Nothing here is allowed to invent charge.
	 */
	i = (m - v) * rat.d_inv_rtot;
	rat.vc7 += sign * i * (1.0f / (RAT_C7 * SAMPLES_PER_SEC));

	/* the node is behind the diode's own resistance, not at it */
	return sign * (v + i * rat.d_rs);
}

float rat_step(float in)
{
	float x, y;

	x = single_pole_hpf(in * RAT_VPEAK, &rat.hp_in, rat.hp_in_c);

	y = rat_opamp(x);
	y = rat_diode_clip(y);

	y = single_pole_lpf(y, &rat.lp_filt, rat.lp_filt_c);
	y = single_pole_hpf(y, &rat.hp_out, rat.hp_out_c);
	return y * rat.kout;
}
