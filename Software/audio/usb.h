//
// usb "effect" - it's just setting the output details
//
enum usb_output {
	LR_Wet, LR_Dry, LR_WetDry
};

struct {
	enum usb_output output;
} usb;

static void usb_init(signed char pot[10])
{
	usb.output = pot[0];
}

static inline float usb_step(float in)
{
	return in;
}

static const char *const usb_output_names[] = { "Wet", "Dry", "Wet/Dry", NULL };

static struct effect usb_effect = {
	.name = "USB",
	.short_name = "USB",
	.init = usb_init,
	.step = usb_step,
	.pots = {
		{ "L/R Out", desc_none, NULL, 0, usb_output_names },
	}
};
