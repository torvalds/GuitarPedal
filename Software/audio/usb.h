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

static void usb_init(signed char pot[10])
{
	usb.output = pot[0];
	usb.input = pot[1];
}

static inline float usb_step(float in)
{
	return in;
}

static const char *const usb_output_names[] = { "Wet", "Dry", "Wet/Dry", NULL };
static const char *const usb_input_names[] = { "Off", "Pre-FX", "Mix", NULL };

static struct effect usb_effect = {
	.name = "USB",
	.short_name = "USB",
	.init = usb_init,
	.step = usb_step,
	.pots = {
		{ "L/R Out", desc_none, NULL, 0, usb_output_names },
		{ "L/R In", desc_none, NULL, 0, usb_input_names },
	}
};
