// NAME: Bluetooth Audio [RADIO]
// PRIORITY: 134
// MIX: NONE		// it isn't an effect, and there is nothing to mix
//
// Kept once rather than per scene, like USB Audio: a backing track
// playing is true of the pedal and not of any one sound.
//
// GLOBAL
// POSITION: BACK
// HW: RADIO
//
// ABOUT: Audio over Bluetooth, both ways - a phone playing to the
// ABOUT: pedal as it would to a headset, and hearing the pedal as the
// ABOUT: headset's microphone. The phone's own volume still works;
// ABOUT: this is the pedal's side of it.
// POT: "In" ENUM(Off Pre-FX Mix Replace) = Pre-FX
// INFO: Where it joins. Pre-FX adds it to the jack, so the effects
// INFO: apply to it too; Mix adds it to the output, after them;
// INFO: Replace ignores the jack entirely.
// POT: "Level" LINEAR(-40.0 6.0) = -12.0 dB
// INFO: How loud it is against the guitar. A phone at full volume
// INFO: arrives about as hot as a guitar does, and a backing track
// INFO: usually wants to sit under it.
// POT: "Out" ENUM(None Wet Dry Wet/Dry) = Wet
// INFO: What the pedal sends over Bluetooth: what a phone hears as
// INFO: the pedal's microphone in a call, and what headphones play.
// INFO: Wet/Dry puts the processed signal on the left and the untouched
// INFO: input on the right. None sends nothing.
//
// The same choices as USB Audio's, in the same stored order.
//
struct {
	enum usb_input input;
	enum usb_output output;
	float level;
} radioaudio;

static void radio_init(unsigned char pot[10])
{
	radioaudio.input = pot[RADIO_IN];
	radioaudio.output = pot[RADIO_OUT];
	radioaudio.level = db_to_level(radio_level_pot(pot));
}

// No audio here either; Audio/effect.h reads the settings.
static inline sample_t radio_step(sample_t in)
{
	return in;
}
