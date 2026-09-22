// NAME: USB Audio [USB]
// PRIORITY: 132
// MIX: NONE		// it isn't an effect, and there is nothing to mix
//
// Kept once rather than per scene: what the host is doing with the pedal
// is true of the pedal and not of any one sound.
//
// GLOBAL
//
// What leaves for the host.
//
// POT: "L/R Out" ENUM(None Wet Dry Wet/Dry) = Wet
// INFO: What the pedal sends back. Wet/Dry puts the processed signal on
// INFO: the left and the untouched input on the right, both from the
// INFO: same instant - which is what lets a capture be compared against
// INFO: its own input with nothing to align.
//
// ...and what arrives from it.
//
// POT: "L/R In" ENUM(Off Pre-FX Mix Replace) = Off
// INFO: What the host's audio does when it arrives. Pre-FX adds it to
// INFO: the jack and Mix adds it to the output; Replace ignores the jack
// INFO: entirely, which is the one to use when measuring, because
// INFO: whatever is plugged in is otherwise part of the answer.
//
// The host's own volume is not here and cannot be: it arrives over the
// USB audio class rather than over MIDI, and the host owns it.
// Firmware/usb-volume.h is where that lands.
//
// The USB audio pseudo-effect - what crosses the wire, in both directions

enum usb_output {
	LR_None, LR_Wet, LR_Dry, LR_WetDry,
};

//
// Appended rather than slotted in beside Pre-FX where it belongs.
//
// The value is stored, and this enum's order is what it is stored as -
// so putting Replace second would silently turn every saved Mix into a
// Replace.  That is issue 71's shape: a checksum that is a plain sum
// cannot tell a reordering from the values it was given.
//
enum usb_input {
	USB_IN_OFF, USB_IN_PRE_FX, USB_IN_MIX, USB_IN_REPLACE
};

struct {
	enum usb_output output;
	enum usb_input input;
} usbaudio;

static void usb_init(unsigned char pot[10])
{
	usbaudio.output = pot[USB_L_R_OUT];
	usbaudio.input = pot[USB_L_R_IN];
}

//
// There is no audio here, and that is the point.  See settings.h, which
// is the same shape and says why at length.
//
static inline sample_t usb_step(sample_t in)
{
	return in;
}
