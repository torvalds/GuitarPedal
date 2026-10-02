// NAME: Hum Filter [DEHUM]
// PRIORITY: 1
// POSITION: FRONT	// its cuts run on the input, ahead of everything
// MIX: NONE		// so there is nothing here to mix
// INIT: core0		// moving Target starts a measurement on core 0
// DRAW: HUMCUTS		// the cuts it measured, which no pot holds
// ABOUT: Removes the hum that single coils and noisy power put on the
// ABOUT: guitar, cutting only the hum's own frequencies so the tone is
// ABOUT: left alone. Switch it on and keep the strings quiet for a few
// ABOUT: seconds while it listens; with no hum it cuts nothing.
// POT: "Target" LINEAR(-115.0 -65.0) = -95.0 dB
// INFO: How quiet the hum is made. All the way down takes it as far as
// INFO: it will go; all the way up only tames the loudest of it. Turning
// INFO: it acts at once, then listens again for a few seconds with your
// INFO: sound on: keep the strings quiet, with the guitar's volume up and
// INFO: standing where you will play. Off and on again keeps what it
// INFO: learned.
//
// The part of the hum cuts the app can see: the target as a pot, and
// being routed as the switch.  The cuts run in Audio/hum.h, called from
// process_input(), and are learnt by Firmware/hum-learn.h.
//

//
// Called on core 0 whenever the pot moves, a scene load included.  It
// only records the target; hum_task() recomputes the cuts for it and
// measures again.
//
static void dehum_prepare(const unsigned char pot[10])
{
	hum_want_target = (unsigned int)(-dehum_target_pot(pot) + 0.5f);
}

//
// Never called: with 'MIX: NONE' nothing calls a step, but the
// generator still declares one, so one has to exist.
//
static inline sample_t dehum_step(sample_t in)
{
	return in;
}
