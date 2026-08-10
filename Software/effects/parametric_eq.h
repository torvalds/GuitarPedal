// NAME: Parametric EQ [EQ]
// PRIORITY: 120
// GRAPH: LOSHELF PEAKING PEAKING PEAKING HISHELF
// POT: "LS Freq" EXPONENTIAL(20.0 20480.0) = 100.0 Hz
// POT: "LS Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P1 Freq" EXPONENTIAL(20.0 20480.0) = 250.0 Hz
// POT: "P1 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P2 Freq" EXPONENTIAL(20.0 20480.0) = 1000.0 Hz
// POT: "P2 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "P3 Freq" EXPONENTIAL(20.0 20480.0) = 4000.0 Hz
// POT: "P3 Gain" LINEAR(-20.0 20.0) = 0.0 dB
// POT: "HS Freq" EXPONENTIAL(20.0 20480.0) = 8000.0 Hz
// POT: "HS Gain" LINEAR(-20.0 20.0) = 0.0 dB
//
// Every band gets the whole audio range, and the order of the five is
// not the pedal's business.
//
// They used to be five overlapping windows - the low shelf stopped at
// 400Hz, the first peak started at 50 - which kept them roughly in order
// by construction, but only roughly: the windows overlap because they
// have to, so a low shelf at 300Hz above a first peak at 100Hz was
// always reachable.  So it enforced nothing while still being in the
// way, and the arithmetic here does not care in the slightest.  Five
// biquads in series commute; a high shelf below the low shelf is a
// perfectly good filter, just an oddly described one.
//
// Keeping them in a sensible order is the app's business, where it is a
// question about what a control should do rather than about the maths.
//
// EXPONENTIAL rather than FREQUENCY, which is a cubic.  A cubic across
// three decades puts nearly all its resolution at the top: it would step
// by 12% at 100Hz, which is two semitones in the register this pedal
// spends its life in.  A log curve steps by the same ratio everywhere,
// and against the fixed Q of 1 below - a band about 1.4 octaves wide -
// that step is a twentieth of the band's own width.  It also matches the
// app's log frequency axis, so a pot step is the same distance on screen
// wherever you are.
//
// 20480 rather than 20000, which is the same number to look at and a
// better one to divide.  20480 is 20 << 10, so the range is exactly ten
// octaves, and 120 pot steps across ten octaves is exactly twelve steps
// to the octave: one step is one semitone, exactly, everywhere.  That
// costs nothing - two significant figures renders both as "20kHz", and
// the top of the range is inaudible either way - and it means a reading
// in note names would be honest rather than drifting by a third of a
// semitone across the range, if one is ever wanted.
//
// Note that this changes what a stored scene means: the eeprom holds pot
// values, not frequencies, and both the range and the curve moved under
// them.  A saved LS Freq of 60 was 67Hz and is now 632Hz.  There is no
// version stamp to migrate on and this is a hobby pedal, so the answer
// is to re-save the scenes.

struct {
	struct biquad_coeff coeff[5];
	struct biquad_state state[5];
} eq;

// Q comes from the GRAPH: line above, so the app draws the same shape
// this builds.  Unstated there means 1.0, which is what these five have
// always used - all ten pots are spent on frequencies and gains, so
// there is nothing left to make it adjustable with.  See tone.h, which
// has the room.
static void eq_init(unsigned char pot[10])
{
	struct biquad_coeff *c = eq.coeff;
	float q[5];

	eq_graph_q(q, pot);

	_biquad_loshelf(c+0, _w0(eq_ls_freq_pot(pot)), q[0], db_to_A(eq_ls_gain_pot(pot)));
	_biquad_peaking(c+1, _w0(eq_p1_freq_pot(pot)), q[1], db_to_A(eq_p1_gain_pot(pot)));
	_biquad_peaking(c+2, _w0(eq_p2_freq_pot(pot)), q[2], db_to_A(eq_p2_gain_pot(pot)));
	_biquad_peaking(c+3, _w0(eq_p3_freq_pot(pot)), q[3], db_to_A(eq_p3_gain_pot(pot)));
	_biquad_hishelf(c+4, _w0(eq_hs_freq_pot(pot)), q[4], db_to_A(eq_hs_gain_pot(pot)));
}

static float eq_step(float in)
{
	const struct biquad_coeff *c = eq.coeff;
	struct biquad_state *s = eq.state;

	float val = _biquad_step(c+0, s+0, in);
	val = _biquad_peaking_step(c+1, s+1, val);
	val = _biquad_peaking_step(c+2, s+2, val);
	val = _biquad_peaking_step(c+3, s+3, val);
	return _biquad_step(c+4, s+4, val);
}
