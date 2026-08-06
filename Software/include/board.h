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
// **Only the pin map is a build option**, and only two of them exist.
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
// Everything below is common to every board and has been through every
// generation unchanged.
//

#define I2S_BCLK		8
#define I2S_FSYNC		9
#define I2S_DIN			10
#define I2S_DOUT		11

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
#if MIDI_HW
#define MIDI_UART		uart1
#endif

#define I2C0_SDA		4
#define I2C0_SCL		5
#define I2C1_SDA		2
#define I2C1_SCL		3

#define MC24Cxx_I2C		i2c0, 0x50
#define TAC5112_I2C		i2c0, 0x51
#define SH1106_I2C		i2c1, 0x3c
