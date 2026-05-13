//
// usb "effect" - it's just setting the output details
//
enum usb_output {
	LR_Wet, LR_Dry, LR_WetDry
};

struct {
	enum usb_output output;
} usb;

// Hackety hack to make save/restore work
static void usb_save(struct effect *effect, signed char pot[10])
{
	pot[3] = effect->active_pot;
}

static void usb_load(struct effect *effect, signed char pot[10])
{
	effect->active_pot = pot[3];
	if (effect->active_pot > 2)
		effect->active_pot = 0;
}

static void usb_init(signed char pot[10])
{
}

static inline float usb_step(float in)
{
	return in;
}

static void usb_graph(struct effect *effect, int active, signed char pots[10])
{
	usb.output = active;

	// Hackety hack to make the UI not show random values
	pots[0] = 0;
	pots[1] = 0;
	pots[2] = 0;

	static const char *state[] = {
		"L/R: Wet    ",
		"L/R: Dry    ",
		"L/R: Wet/Dry",
	};

	if (active >= ARRAY_SIZE(state))
		return;

	sh1106_puts_6x8(20,20, state[active]);
}

static struct effect usb_effect = {
	.name = "USB",
	.short_name = "USB",
	.init = usb_init,
	.step = usb_step,
	.graph = usb_graph,
	.load = usb_load,
	.save = usb_save,
	.pots = {
		{ "Wet", desc_none, },
		{ "Dry", desc_none, },
		{ "Wet/Dry", desc_none, },
	}
};
