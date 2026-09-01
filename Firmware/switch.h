//
// The switches, as software sees them.
//
// One bit per switch in 'switch_val', set by the debounce PIO
// interrupt and cleared by whoever acts on it.  A long press sets a
// second bit LONGPRESS_SHIFT higher instead of the short one.
//
// The switch id is the bit number *and* the PIO state machine index -
// switch_gpio[] below is what ties an id to a pin, and init_sw_pins()
// walks it in order, so a switch cannot end up reading the wrong pin
// without the table saying so.  It used to be a set of bare numbers
// spread across three files, where switch_pressed(4) tested a bit that
// nothing ever set and nobody noticed.
//
// The two on the expression jack are only switches while something says
// they are - see init_exp_switches().  They take the last two of pio1's
// four state machines, which is the ceiling on this scheme.
enum switch_id {
	ROTARY_SWITCH,		// the rotary encoder's shaft, pressed down
	STOMP_SWITCH,		// the footswitch
	NR_ONBOARD_SWITCHES,	// the ones that are soldered down
#ifdef EXP_TIP_GPIO
	EXP_TIP_SWITCH = NR_ONBOARD_SWITCHES,	// an accessory's, on the tip
	EXP_RING_SWITCH,			// and on the ring
#endif
	NR_SWITCHES,
};

static const unsigned char switch_gpio[NR_SWITCHES] = {
	[ROTARY_SWITCH]	= ROTARY_SW_GPIO,
	[STOMP_SWITCH]	= STOMP_GPIO,
#ifdef EXP_TIP_GPIO
	[EXP_TIP_SWITCH]	= EXP_TIP_GPIO,
	[EXP_RING_SWITCH]	= EXP_RING_GPIO,
#endif
};

//
// Short presses live in the low bits, long presses the same distance
// up.  Both have to fit in 'switch_val'.
//
#define LONGPRESS_SHIFT 16
#define LONGPRESS(sw) ((sw) + LONGPRESS_SHIFT)

_Static_assert(NR_SWITCHES <= LONGPRESS_SHIFT,
	       "switches and their long presses overlap");
_Static_assert(LONGPRESS(NR_SWITCHES) <= 32,
	       "switch_val is too narrow for this many switches");

static unsigned int switch_val;

#define switch_pressed(sw) (!!(switch_val & (1u << (sw))))
#define switch_clear(sw) __atomic_and_fetch(&switch_val, ~(1u << (sw)), __ATOMIC_RELAXED)
