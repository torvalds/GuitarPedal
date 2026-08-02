// NAME: Signal Chain [CHAIN]
// PRIORITY: 0 (Special: always runs first, and is never in effect_chain)
// MIX: NONE		// not an effect - it is the two ends of the chain
// POT: "Gate" LINEAR(-100.0 -40.0) = -70.0 dB
// INFO: Everything quieter than this is silenced. Set it just above the
// INFO: noise floor shown below. Fully down switches the gate off.
// POT: "Attack" LINEAR(0.0 10.0) = 1.5 ms
// INFO: How fast the gate opens once you play. Too slow eats the pick
// INFO: attack; too fast lets the noise through between notes.
// POT: "Release" LINEAR(50.0 500.0) = 150.0 ms
// INFO: How long it waits before closing again. Too short chops the
// INFO: tail off a chord as it decays.
// POT: "Trim" LINEAR(-20.0 20.0) = 0.0 dB
// INFO: Brings your pickup up to the level the rest of the chain is
// INFO: calibrated for, so every effect's dB markings mean what they
// INFO: say. Set it once, by the meters, and leave it.
// POT: "Volume" LINEAR(-40.0 20.0) = 0.0 dB
// INFO: How loud the pedal is, applied right at the end and changing
// INFO: nothing about the sound. Wanting more out of the pedal is not
// INFO: a reason to turn Trim up. Fully down is silence.
//
// The beginning and the end of the signal chain.
//
// This is not an effect and does not pretend to be one.  It is never in
// effect_chain[], ROUTABLE_EFFECTS excludes it, single_sample() calls it
// directly rather than through do_effect_step(), and it has no wet and no
// dry - a gain stage cannot be blended against itself, because half of a
// gain plus half of no gain is a different gain rather than less of one.
// Hence 'MIX: NONE'.
//
// Trim and Volume are the two ends of it, and they are two controls
// rather than one because the chain is full of things that are not
// linear.  Where you sit on a triode curve, where the boost starts
// folding, how far over the compressor's threshold you are - all of that
// depends on the absolute level going in, and the whole DSP is calibrated
// to a 1Vrms internal scale (see process.h) that any given guitar may
// miss by 20dB in either direction.  Trim is what puts a pickup onto
// that scale, once, so that every effect's dB markings mean what their
// author intended.  Volume is then what makes it as loud as you want
// without disturbing any of it - which is why it goes above unity too,
// and not only down: needing more out of the pedal is not a reason to
// drive the chain harder.
//
// Turn Trim up and Volume down by the same amount and you hear the
// non-linearity by itself with the loudness held still, which is the only
// honest way to judge it - louder always sounds better.
//
// The gate measures ahead of Trim deliberately - see chain_step()
// for why.  Trim is about the chain that follows; the noise the gate
// exists to cut is a property of the room and the pickup, and does not
// move when trim does.
//
// Note that it can only be about noise arriving at the *input*.  A gate
// at the front of the chain can do nothing about hiss produced further
// down it, which is why real rigs often put one in an amp's effects
// loop.  That costs us nothing today, because the chain is float
// arithmetic and adds no noise of its own - the converter's floor is the
// only floor there is.  If a preamp model ever gains modelled noise, the
// answer is a second gate at the far end rather than moving this one.
//
// This leaves 'mix', 'target', 'dry' and 'wet' unused on this effect.
// reset_effect() and the eeprom loader still set them; nothing ever reads
// them.  Not worth special-casing the loaders over.
//
// Same envelope calculations as the compressor. I wonder if
// I should just have a combined noise gate / compressor thing?
//
// But the attack/release values are probably different.
//

static struct {
	struct envelope envelope;
	float mult, level;
	bool active;

	//
	// Both gains are slewed rather than applied as they arrive.
	// Volume is CC 7, so it can be under a fader or an expression
	// pedal and move a long way in a hurry, and a gain that steps
	// instead of gliding clicks.  Trim moves rarely and costs the
	// same to do properly.
	//
	float trim, trim_target;
	float volume, volume_target;
} chain;

// How fast the two gains chase their target: ~10ms at 48kHz, as MIX_SLEW
#define CHAIN_SLEW (1.0f / 512)

static inline void chain_init(unsigned char pot[10])
{
	chain.trim_target = db_to_level(chain_trim_pot(pot));

	//
	// Gate fully down is off, and not merely very quiet.
	//
	// The bottom of the travel is -100dBV, which is 28uV peak to peak.
	// No signal chain is that clean, so a threshold set there would
	// never close the gate anyway and switching it off changes nothing
	// you can hear.  What it does change is what gets *reported*: the
	// telemetry sends the gate multiplier, and a gate that is off has
	// to say 1.0 rather than whatever it last ramped to.  So make it
	// an explicit off internally, and let the one control mean both
	// things - a knob whose bottom end is "not at all" is what people
	// expect, and it saves an on/off switch that only ever agreed with
	// where the knob already was.
	//
	chain.active = pot[CHAIN_GATE] != 0;

	float level_db = chain_gate_pot(pot);
	chain.level = db_to_level(level_db);

	float attack_ms = chain_attack_pot(pot);
	float release_ms = chain_release_pot(pot);
	envelope_init(&chain.envelope, attack_ms, release_ms);

	//
	// Volume reads in dB and goes above unity as well as below,
	// because it is the output level and not an attenuator: wanting
	// more out of the pedal should not mean driving the chain harder,
	// which is Trim's job and changes the sound.
	//
	// The bottom of the travel is silence rather than -40dB, so that
	// CC 7 at zero means what a MIDI host means by it.  -40dB is a
	// hundredth of an amplitude, so the step from there to nothing is
	// inaudible - which is why the range goes that low rather than
	// stopping at the -20dB that would otherwise be plenty.
	//
	chain.volume_target = pot[CHAIN_VOLUME] ?
		db_to_level(chain_volume_pot(pot)) : 0.0f;
}

//
// The front of the chain: the gate, and the trim.
//
// The envelope is measured on the *untrimmed* input, which is the whole
// point of the ordering.  Both trim and the gate's ramp are scalar
// multiplies, so they commute and the audio comes out identical either
// way - the only thing the order decides is what the threshold is
// compared against, and trim moves the signal and the noise together.
// Measured after trim, a threshold sitting correctly just above the hum
// would need raising by exactly however much trim was raised, every
// time, which is a control whose only job is to undo another one.
// Measured before, it depends on the room, the pickup and the converter,
// none of which trim can reach.  Which is what a noise gate is about,
// and why 'Gate' reads in dBFS rather than as a fraction of anything.
//
// Also where Volume is slewed, even though single_sample() is what
// applies it at the far end.  This runs once per sample and that is all a
// slew needs, and it beats teaching the tail of the chain how to chase a
// target of its own.
//
static inline sample_t chain_step(sample_t in)
{
	chain.trim += (chain.trim_target - chain.trim) * CHAIN_SLEW;
	chain.volume += (chain.volume_target - chain.volume) * CHAIN_SLEW;

	float gain = chain.trim;

	//
	// The envelope follows the left channel - the guitar - but the gate
	// closes on both, so a stereo signal survives it.
	//
	// Run unconditionally, and only *use* it when the gate is on.  It
	// costs one multiply-add, and the alternative is that the reported
	// noise floor freezes at whatever it was when the gate was switched
	// off - the floor meter is derived from this and from nothing else,
	// deliberately, so that the number on screen is the same quantity
	// 'Gate' gets compared against.  See single_sample().
	//
	float env = envelope_step(&chain.envelope, in.left);

	if (chain.active) {
		float mult = chain.mult;

		// Ramp up fairly quickly, ramp down slowly
		if (env >= chain.level) {
			mult = linear(0.01f, mult, 1.0f);
			if (mult > 0.99f)
				mult = 1.0f;
		} else {
			mult = linear(0.001f, mult, 0.0f);
			if (mult < 0.01f)
				mult = 0.0f;
			chain_effect.intense = 1;
		}
		chain.mult = mult;
		gain *= mult;
	}

	in.left *= gain;
	in.right *= gain;
	return in;
}
