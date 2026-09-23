// NAME: Analog In Tone [INTONE]	POSITION: FRONT
// NAME: Analog Out Tone [OUTTONE]	POSITION: BACK
// PRIORITY: 133
// MIX: NONE		// it runs in the codec, not in the chain
// INIT: core0		// and core 0 is what talks to the codec
// HW: CODEC_DSP
// GRAPH: LOSHELF:0.707 PEAKING:MID_Q HISHELF:0.707
// POT: "Bass Freq" EXPONENTIAL(20.0 20480.0) = 200.0 Hz
// POT: "Bass" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Mid Freq" EXPONENTIAL(20.0 20480.0) = 800.0 Hz
// POT: "Mid" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Treble Freq" EXPONENTIAL(20.0 20480.0) = 3000.0 Hz
// POT: "Treble" LINEAR(-15.0 15.0) = 0.0 dB
// POT: "Mid Q" EXPONENTIAL(0.3 4.0) = 0.707
//
// The same three bands as [TONE], run by the codec's own biquads rather
// than by us.  Two of them: the converter has three sections per channel
// on the way in and three more on the way out, and the names say which
// is which.
//
// They cost the audio core nothing at all.  There is no step() worth the
// name, because by the time a sample reaches our code the record
// sections have already filtered it, and when one leaves the playback
// sections have not filtered it yet.
//
// Stereo, which [TONE] is not.  Both channels get the same coefficients
// and each keeps its own delay line, so a stereo signal comes out with
// its two sides intact.  A mono effect is wrapped as
// 'val.left = val.right = step(val.left)', so one [TONE] across a stereo
// signal collapses it; shaping both sides in the chain takes two of
// them, steered one to each channel.
//
// Analog is the operative word, and it is why the name carries it.  They
// are the first and last things in the signal and they sit outside
// everything USB audio can reach: what the host records has been through
// the input one and not the output one, and what the host sends has been
// through neither on the way in and both on the way out.  Nothing about
// that is visible from these controls, which is why usb.h says it where
// it bites.
//
// Fixed point, where the chain is float, and that is the one way these
// behave differently from [TONE].  An effect in the chain can push a
// signal far past full scale and have a later one, or the master
// volume, bring it back; nothing can undo a clip that happened in the
// converter.  Overflow saturates rather than wrapping - measured, at
// +15dB into a hot input nearly half the samples sit at exactly full
// scale and not one crosses to the other rail - so it runs out of
// headroom the way a stage does.  Set a boost here for the level going
// in, not for the level you want out.
//
// Pinned to the ends of the chain because that is where they are.  They
// are routed like any other effect - being in the chain is the whole of
// being on - but there is nowhere else they could sit, so there is no
// handle to drag.  Drawn either side of [CHAIN] rather than strictly
// around it: the record sections run ahead of the trim and the gate, so
// the gate hears the tone, which is worth knowing and not worth
// reordering the display over.
//
// Bypass has to reach them by hand.  The crossfade in single_sample()
// sits between the record sections and the playback ones, so it cannot
// take either out, and switching one off means walking its bands down
// to 0 dB rather than leaving the codec alone.
//
// 'HW: CODEC_DSP' is the i2c probe.  A board whose codec is strapped
// rather than programmed has no biquads to offer, and the app is told
// not to draw these at all rather than draw controls that do nothing.
//
// 'INIT: core0' is the whole reason that declaration exists.  An
// effect's init() is audio-core code and may not reach a bus, so the
// coefficients are worked out in prepare(), on the core that writes
// them.  There is no init() here at all: nothing is waiting on the
// audio core, because the filter is in the codec.
//
// The pot accessors below wear the first name's prefix because they are
// shared - one set of pots, read the same way by both.

#include "hwtone.h"

static struct hwtone SELF(_state);

static void SELF(_prepare)(const unsigned char pot[10])
{
	struct hwtone *ht = &SELF(_state);
	float q[3];

	intone_graph_q(q, pot);
	hwtone_set_band(ht, HWTONE_BASS, intone_bass_freq_pot(pot), q[0],
			intone_bass_pot(pot));
	hwtone_set_band(ht, HWTONE_MID, intone_mid_freq_pot(pot), q[1],
			intone_mid_pot(pot));
	hwtone_set_band(ht, HWTONE_TREBLE, intone_treble_freq_pot(pot), q[2],
			intone_treble_pot(pot));
}

//
// Never called.  'MIX: NONE' is why: the generator emits no .step for
// such an effect, and there would be nothing for one to do anyway.  It
// is written out because the generator declares one regardless.
//
static inline sample_t SELF(_step)(sample_t in)
{
	return in;
}
