// NAME: Settings [SETTINGS]
// PRIORITY: 130
// MIX: CUSTOM		// it isn't an effect, and there is nothing to mix
// POT: "USB L/R Out" ENUM(None Wet Dry Wet/Dry) = Dry
// POT: "USB L/R In" ENUM(Off Pre-FX Mix) = Off
// POT: "MIDI Ch" ENUM(Omni Ch1 Ch2 Ch3 Ch4 Ch5 Ch6 Ch7 Ch8 Ch9 Ch10 Ch11 Ch12 Ch13 Ch14 Ch15 Ch16) = Omni
// POT: "LED" LINEAR(0 100) = 10 %
// POT: "  ATTN" LINEAR(0 100) = 50 %
// POT: "Tuning" ENUM(EADGBE DADGAD BEADGC EADG) = EADGBE
//
// Settings "effect" - dummy effect to save various settings
//
enum usb_output {
	LR_None, LR_Wet, LR_Dry, LR_WetDry,
};

enum usb_input {
	USB_IN_OFF, USB_IN_PRE_FX, USB_IN_MIX
};

struct {
	enum usb_output usb_output;
	enum usb_input usb_input;
	int midi_channel;
	float led_pwm, led_intense;
	int tuning;
} settings;

//
// Which pot is which.  These have to stay in step with the POT lines
// at the top of this file - that is the order gen_effects.py assigns.
//
enum settings_pot {
	SETTINGS_USB_OUT,
	SETTINGS_USB_IN,
	SETTINGS_MIDI_CH,
	SETTINGS_LED,
	SETTINGS_ATTN,
	SETTINGS_TUNING,
};

static void settings_init(unsigned char pot[10])
{
	settings.usb_output = pot[SETTINGS_USB_OUT];
	settings.usb_input = pot[SETTINGS_USB_IN];
	settings.midi_channel = pot[SETTINGS_MIDI_CH];

	settings.led_pwm = settings_pot3(pot[SETTINGS_LED]) / 100;

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
	float intense = settings_pot4(pot[SETTINGS_ATTN]) / 100;
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
// It cannot even be routed: ROUTABLE_EFFECTS in blink.c is the bits
// between the noise gate and this, so neither end can be put in a
// chain, and this function is unreachable by construction.  Declaring
// it 'MIX: CUSTOM' says so in the one place a reader will look - the
// multipliers arrive and are ignored, because a wet/dry blend of a
// thing that makes no sound has nothing to say.
//
// It also stops init() from having to lie about being enabled to keep
// itself scheduled.  See make_one_noise().
//
static inline sample_t settings_step(sample_t in, float dry, float wet)
{
	return in;
}
