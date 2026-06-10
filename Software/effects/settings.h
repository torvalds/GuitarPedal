// NAME: Settings [SETTINGS]
// PRIORITY: 130
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
	int screensaver;
	int tuning;
} settings;

static void settings_init(unsigned char pot[10])
{
	settings.usb_output = pot[0];
	settings.usb_input = pot[1];
	settings.midi_channel = pot[2];

	settings.led_pwm = settings_pot3(pot[3]) / 100;
	settings.led_intense = settings_pot4(pot[4]) / 100;

	// Hacky hacky
	settings_effect.target = EFF_ENABLE_STEPS;
	settings_effect.intense = settings_effect.active_pot == 4;

	settings.tuning = pot[5];
}

static inline float settings_step(float in)
{
	return in;
}
