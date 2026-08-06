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
// The pedal as it stands has one rotary encoder - and most boards do
// not even populate that - one stomp switch, and one LED.
//

//
// Two boards, two completely different pin maps.
//
// This wants to be a proper build option rather than a define somebody
// edits - see the issue list - but the two maps have to be written down
// before that can be built on top of them, and getting it wrong is not
// subtle: on usb-stomp, GPIO0 is the stomp switch shorting to ground,
// and the old map drives it as an LED output.
//
// The symptom that found it was not a dead LED.  GPIO13 is the rotary
// encoder's B line here and the old map reads it as the stomp switch,
// so wherever the knob is parked the firmware sees a switch held down
// forever, fires pot actions forever, and every LED sits white at the
// attention brightness.  A wrong pin map does not look like a wrong pin
// map.
//
#define BOARD_USB_STOMP		1

#if BOARD_USB_STOMP

//
// No plain LED at all.  There is one on the board, but it is across +5V
// through a resistor - a power indicator, not something addressable.
// The three WS2812Bs below are the only LEDs the firmware has.
//
#define ROTARY_A_GPIO		12
#define ROTARY_B_GPIO		13
#define ROTARY_SW_GPIO		14
#define STOMP_GPIO		0

#else

// The one LED.  Plain PWM brightness; see set_led().
#define LED_GPIO		0

//
// The one rotary encoder: A and B are the quadrature pair, SW is the
// shaft pressing down.
//
#define ROTARY_A_GPIO		6
#define ROTARY_B_GPIO		7
#define ROTARY_SW_GPIO		12

// The one stomp switch.  Internal pull-up, closing to GND.
#define STOMP_GPIO		13

#endif

#define I2S_BCLK		8
#define I2S_FSYNC		9
#define I2S_DIN			10
#define I2S_DOUT		11

//
// Hardware MIDI.  Both boards are on uart1 - which is not obvious, and
// getting it wrong costs an evening, so the funcsel is spelled out here
// rather than assumed in the code that uses it.
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
  #if BOARD_USB_STOMP
    // Out through an AHCT buffer at 5V, in through a TLP2310 optocoupler.
    #define MIDI_OUT		20
    #define MIDI_IN		21
    #define MIDI_FUNCSEL	2
  #else
    // On the pins the second rotary encoder used to have
    #define MIDI_OUT		26
    #define MIDI_IN		27
    #define MIDI_FUNCSEL	11
  #endif
  #define MIDI_UART		uart1
#endif

#define I2C0_SDA		4
#define I2C0_SCL		5
#define I2C1_SDA		2
#define I2C1_SCL		3

#define MC24Cxx_I2C		i2c0, 0x50
#define TAC5112_I2C		i2c0, 0x51
#define SH1106_I2C		i2c1, 0x3c

//
// The smart LEDs.  Three WS2812B-2020 in a chain off one pin, driven
// straight from 3.3V logic - modern parts take 0.55*VSS as a high, and
// these do; older ones and clones want a level shifter.
//
// GPIO 25 is not a leftover: it is what the usb-stomp board wires them
// to, and the knob test board independently arrived at the same pin.
//
// Nothing on this pin has anything to do with the plain LED that used to
// be the only one.  That one is still on the board but it is wired
// across +5V through a resistor and is a power indicator, not something
// the firmware can address.
//
#define WS2812_GPIO		25
#define NR_LEDS			3

//
// Pins that used to be something and no longer are, recorded so nobody
// has to go through the git history to find out why there are gaps:
//
//	GPIO 1		second LED (now WS2812_GPIO, above)
//	GPIO 26/27	second rotary's quadrature pair (now hardware MIDI)
//	GPIO 28		second rotary's shaft switch
//	GPIO 29		second stomp switch
//
