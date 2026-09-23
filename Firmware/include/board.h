//
// Board GPIO pin definitions
//
// Named for what the pin does, not for the order someone happened to
// wire it up in.  The board has been through several generations and
// the old numbering survived none of them: "SW1" was the first rotary's
// shaft switch, "SW3" was the first stomp, and SW2/SW4 were a second
// rotary and a second stomp that no longer exist.  None of that was
// visible in the names.
//

//
// Which board this build is for.
//
// PEDAL_BOARD_HEADER comes from cmake, which will not configure without
// a board named - there is deliberately no default.  Every board is a
// separate target that is built and named separately, so the answer is
// picked once in board.local and the artifacts say which is which; see
// the comment at the top of CMakeLists.txt.
//
// **Only the pin map is a build option**, and there are three of them.
// Everything else that varies between boards - which codec, whether a
// rotary or the MIDI jacks are fitted - either does not reach the pins
// or is discovered at runtime, and a thing the firmware can find out for
// itself has no business being a build option.
//
// The pin map is not in that category and cannot be.  The unified board
// and the split boards share four GPIOs out of the lot, and a wrong map
// does not look like a wrong map: on unified GPIO0 is the stomp switch,
// shorting to ground with no series resistor, and the split map drives
// it as an LED output.  The symptom that actually found it was stranger
// still - GPIO13 is the encoder's B line on unified and the split map
// reads it as the stomp, so wherever the knob was parked the firmware
// saw a switch held down for ever, fired pot actions for ever, and every
// LED sat white at the attention brightness.
//
// So the build is named and the binary carries the name, in the image
// for picotool and in the USB product string for everything else - see
// usb-device.c.
//
#include PEDAL_BOARD_HEADER

//
// Everything below is common to every board.  The i2s pins, the i2c pins
// and the codec's address used to be here too, on the strength of having
// been through every generation unchanged; the minimal board moved all
// three, so they live with the rest of the pins now.
//

//
// Hardware MIDI is on uart1 on every board - which is not obvious, and
// getting it wrong costs an evening, so the funcsel is spelled out with
// the pins in each board file rather than assumed in the code that uses
// them.
//
// The RP2350 offers UART1 TX/RX on 20/21 at function *2* and on 26/27
// at function *11*, because 26/27 have UART1 CTS/RTS at 2 and the TX/RX
// pair is one of the extended functions.  So the number is a property
// of the pins, not of the UART, and it lives with them.
//
// 2 is what GPIO_FUNC_UART is, so 20/21 want nothing special at all -
// which was the point of moving to them.  The 11 for 26/27 is the magic
// the SDK has no name for, and hardcoding it in uart.h is what stopped
// the simpler pins from being simpler.
//
// Read as a value as well as tested, so every board has to have one.
#ifndef MIDI_HW
#define MIDI_HW			0
#endif

#if MIDI_HW
#define MIDI_UART		uart1
#endif

//
// What an effect's 'HW:' line is asking about.
//
// One per capability an effect can declare, named HAVE_<what it said>,
// and the whole point is that the two kinds of answer look the same
// from the effect's side.  EXPRESSION is a pin that either exists on
// this board or does not, and folds away; CODEC_DSP is a chip that has
// to be asked, and cannot.
//
// Here rather than in the per-board files because both are derived
// from something those files already say.  A capability no board can
// derive would belong there instead, the way MIDI_HW does.
//
#ifdef EXP_TIP_GPIO
#define HAVE_EXPRESSION		1
#else
#define HAVE_EXPRESSION		0
#endif

#define HAVE_CODEC_DSP		(hardware.i2c_codec)

//
// The i2c devices that are not the codec.  Both are on i2c1, which only
// the boards with a screen header ever wired, so both are conditional on
// its pins existing.  The codec's own address is a board fact and lives
// in the board file.
//
#ifdef I2C1_SDA
#define SH1106_I2C		i2c1, 0x3c
#endif
