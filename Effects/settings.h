// NAME: Settings [SETTINGS]
// PRIORITY: 130
// MIX: NONE		// it isn't an effect, and there is nothing to mix
//
// Kept once rather than per scene: what it holds is true of the pedal
// and not of any one sound.
//
// GLOBAL
// POSITION: BACK
//
// The input jack, and what to do about the channel it is not carrying.
//
// A TS plug shorts the ring to sleeve, so the jack carries one signal
// and the right arrives as silence.  Mono says so, and the right becomes
// a copy of the left before anything else reads it - which is what a
// bypassed pedal needs to come out of both sides of a headphone jack,
// and what a split path needs if it is to have two branches rather than
// one and an absence.
//
// Stereo is a TRS cable or a Y-splitter, where the right is a channel
// rather than a missing one.
//
// Mono by default because a guitar cable is a TS cable.  It costs a
// stereo source its right channel, which is the other way round from
// what it costs a guitar, and only one of the two arrives by accident.
//
// ABOUT: What is true of the pedal rather than of any one sound,
// ABOUT: so these are kept once and not per scene.
// POT: "Analog In" ENUM(Stereo Mono) = Mono
// INFO: A TS guitar cable carries one channel, so Mono copies it to both
// INFO: before the chain - otherwise a bypassed pedal is silent on one
// INFO: side of a headphone jack. Stereo is for a TRS cable or a
// INFO: splitter, where the right channel is really there.
// POT: "MIDI Ch" ENUM(Omni Ch1 Ch2 Ch3 Ch4 Ch5 Ch6 Ch7 Ch8 Ch9 Ch10 Ch11 Ch12 Ch13 Ch14 Ch15 Ch16) = Omni
// INFO: Which MIDI channel the pedal listens on for bypass, the
// INFO: tuner and scene changes. Omni is all of them. Parameter
// INFO: edits from this app arrive either way, which is how it can
// INFO: still fix this when it is wrong.
// POT: "LED" LINEAR(0 100) = 10 %
// INFO: How bright the status LED sits normally. Down for a dark
// INFO: stage, up for daylight.
// POT: "  ATTN" LINEAR(0 100) = 50 %
// INFO: How bright the LED goes when something wants you to notice -
// INFO: clipping, or a compressor working hard. It flashes at this
// INFO: setting while you move it, because the LED is the only thing
// INFO: that can show it.
// POT: "Tuning" ENUM(EADGBE DADGAD BEADGC EADG) = EADGBE
// INFO: Which strings the tuner names. It still hears any note; this
// INFO: is what it expects them to be.
//
// Which pot the app has to be able to find rather than merely show.  It
// filters Control Change and Program Change by this, so the app has to
// transmit on the same one or bypass, the tuner and scene changes stop
// arriving - and it used to find the pot by matching the label above,
// which made renaming that label a silent way to break it.
//
// ROLE: CHANNEL:MIDI_CH
//
// Settings "effect" - dummy effect to save various settings
//
enum analog_in {
	ANALOG_IN_STEREO, ANALOG_IN_MONO
};

struct {
	enum analog_in analog_in;
	int midi_channel;
	float led_pwm, led_intense;
	int tuning;
} settings;

static void settings_init(unsigned char pot[10])
{
	settings.analog_in = pot[SETTINGS_ANALOG_IN];
	settings.midi_channel = pot[SETTINGS_MIDI_CH];

	settings.led_pwm = settings_led_pot(pot) / 100;

	//
	// The attention brightness is the one setting whose effect you
	// cannot see while you are setting it, because the only thing
	// that can show it is the LED going into that mode.  So ask it
	// to, for half a second, whenever the setting moves - or for as
	// long as this is the pot being edited from the pedal itself,
	// where you are looking at the LED and not at a browser.
	//
	// This is also what the second LED used to do, back when there
	// was one to do it with.
	//
	float intense = settings_attn_pot(pot) / 100;
	if (intense != settings.led_intense ||
	    settings_effect.active_pot == SETTINGS_ATTN)
		attention_preview = ATTENTION_PREVIEW_TICKS;
	settings.led_intense = intense;

	settings.tuning = pot[SETTINGS_TUNING];
}

//
// There is no audio here, and that is the point.
//
// This exists to carry settings, and init() above is the whole of it.
// It cannot even be routed: 'GLOBAL' above implies 'ALWAYS', which
// ROUTABLE_EFFECTS in effect-state.h masks out, so this function is
// unreachable by construction.  Declaring it 'MIX: NONE' says so in
// the one place a reader will look, and means nothing generates a mixing
// wrapper for a thing that makes no sound.
//
// It also stops init() from having to lie about being enabled to keep
// itself scheduled.  See make_one_noise().
//
static inline sample_t settings_step(sample_t in)
{
	return in;
}
