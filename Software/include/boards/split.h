//
// The split boards.  Hardware/audio-jacks plus an MCU board.
//
// "Split" is the architecture: an MCU board and an audio-jacks board
// joined by a 12P 0.5mm FFC carrying power, i2c and i2s.  It was done
// that way to lay the two out independently, to fit a 125B enclosure
// with the jacks, rotaries and the original screen all in one top area,
// and out of a worry about noise isolation that never had to be tested.
//
// Both halves are finished hardware now and neither will change again.
// They stay buildable because the basics have not moved and they are
// still useful for testing.
//
// **One board file covers all of them, and that is the point.**  There
// are two MCU boards - one with a rotary encoder, one with hardware MIDI
// jacks - and their pins do not clash, so a single build drives either.
// Polling a rotary that is not fitted reads a pin that never moves, and
// bringing up a UART whose jacks are absent talks to nobody.
//
// The audio boards vary too - a TAC5112 that was never wired for stereo,
// and a TAC5242 that was - and that difference is real and user-visible.
// It is not a build option either: the TAC5112 needs i2c setup anyway,
// so the firmware has to find out which one is there, and having found
// out it can simply say so.  See probe_hardware().
//
// Any audio board pairs with any MCU board; it is a cable.  So nothing
// here may assert what is on the other end of it.
//

// The one LED.  Plain PWM brightness; see set_led().
#define LED_GPIO		0

//
// The one rotary encoder: A and B are the quadrature pair, SW is the
// shaft pressing down.  Not fitted on the MIDI MCU board.
//
#define ROTARY_A_GPIO		6
#define ROTARY_B_GPIO		7
#define ROTARY_SW_GPIO		12

// The one stomp switch.  Internal pull-up, closing to GND.
#define STOMP_GPIO		13

//
// Hardware MIDI, on the pins the second rotary encoder used to have.
// Not fitted on the rotary MCU board.
//
// Funcsel 11 rather than 2: UART1 TX/RX is one of the RP2350 extended
// functions on 26/27, because CTS/RTS already occupy function 2 there.
// See board.h, which explains why that number lives with the pins.
//
#define MIDI_HW			1
#define MIDI_OUT		26
#define MIDI_IN			27
#define MIDI_FUNCSEL		11

//
// Pins that used to be something on these boards and no longer are,
// recorded so nobody has to go through the git history to find out why
// there are gaps:
//
//	GPIO 1		second LED
//	GPIO 28		second rotary's shaft switch
//	GPIO 29		second stomp switch
//
