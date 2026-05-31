// NAME: USB [USB]
// PRIORITY: 130
// POT: "L/R Out" ENUM(Wet Dry Wet/Dry) = Wet
// POT: "L/R In" ENUM(Off Pre-FX Mix) = Off
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
} usb;

static void usb_init(unsigned char pot[10])
{
	usb.output = pot[0];
	usb.input = pot[1];
}

static inline float usb_step(float in)
{
	return in;
}
