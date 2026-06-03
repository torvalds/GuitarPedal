// NAME: USB [USB]
// PRIORITY: 130
// POT: "L/R Out" ENUM(Wet Dry Wet/Dry) = Wet
// POT: "L/R In" ENUM(Off Pre-FX Mix) = Off
// POT: "MIDI Ch" ENUM(Omni Ch1 Ch2 Ch3 Ch4 Ch5 Ch6 Ch7 Ch8 Ch9 Ch10 Ch11 Ch12 Ch13 Ch14 Ch15 Ch16) = Omni
//
// usb "effect" - it's just setting the output details
//
enum usb_output {
	LR_Wet, LR_Dry, LR_WetDry
};

enum usb_input {
	USB_IN_OFF, USB_IN_PRE_FX, USB_IN_MIX
};

struct {
	enum usb_output output;
	enum usb_input input;
	int midi_channel;
} usb;

static void usb_init(unsigned char pot[10])
{
	usb.output = pot[0];
	usb.input = pot[1];
	usb.midi_channel = pot[2];
}

static inline float usb_step(float in)
{
	return in;
}
