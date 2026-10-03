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
// ABOUT: Audio received over Bluetooth - a phone playing to the
// ABOUT: pedal as it would to earbuds. The phone's own volume still
// ABOUT: works; this is the pedal's side of it.
// POT: "In" ENUM(Off Pre-FX Mix Replace) = Pre-FX
// INFO: Where it joins. Pre-FX adds it to the jack, so the effects
// INFO: apply to it too; Mix adds it to the output, after them;
// INFO: Replace ignores the jack entirely.
// POT: "Level" LINEAR(-40.0 6.0) = -12.0 dB
// INFO: How loud it is against the guitar. A phone at full volume
// INFO: arrives about as hot as a guitar does, and a backing track
// INFO: usually wants to sit under it.
//
// The same choices as USB Audio's input, in the same stored order.
//
struct {
	enum usb_input input;
	float level;
} radioaudio;

static void radio_init(unsigned char pot[10])
{
	radioaudio.input = pot[RADIO_IN];
	radioaudio.level = db_to_level(radio_level_pot(pot));
}

// No audio here either; Audio/effect.h reads the settings.
static inline sample_t radio_step(sample_t in)
{
	return in;
}
