//
// The unified board.  Hardware/usb-stomp.
//
// One board instead of two: the rotary encoder *and* the MIDI jacks that
// the split boards each had only one of, plus an expression jack that is
// new here, plus three addressable LEDs where there was one PWM one.
// First board built from design blocks rather than the daughtercard.
//
// It shares four GPIOs with the split map and nothing else, which is
// what needing the ADC pins for the expression jack costs.  Getting it
// wrong is not subtle - see board.h.
//
// The Hardware/ project is still called usb-stomp, for a historical
// reason that has stopped being interesting: every board has always had
// a USB connector and a stomp switch, but on the older ones the USB was
// an internal header for programming the MCU, and this was the one that
// brought it out to the panel.  The name records that and nothing else.
//

#define MIDI_HW			1

//
// No plain LED at all.  There is one on the board, but it is across +5V
// through a resistor - a power indicator, not something addressable.
// The three WS2812Bs below are the only LEDs the firmware has, which is
// why LED_GPIO is absent rather than zero: hardware.h keys the whole PWM
// path off whether WS2812_GPIO exists.
//
#define ROTARY_A_GPIO		12
#define ROTARY_B_GPIO		13
#define ROTARY_SW_GPIO		14
#define STOMP_GPIO		0

//
// Hardware MIDI out through an AHCT buffer at 5V, in through a TLP2310
// optocoupler.
//
// Funcsel 2 rather than the 11 the split boards need: on 20/21 UART1
// TX/RX *is* function 2, which is plain GPIO_FUNC_UART and wants no
// special case at all.  That was the point of moving to these pins.
//
#define MIDI_OUT		20
#define MIDI_IN			21
#define MIDI_FUNCSEL		2

//
// The expression jack.  A TRS jack, and both signal contacts reach the
// chip the same way:
//
//	J103.T --- R105 1k --- GPIO27/ADC1 --- C107 22nF --- GND
//	J103.R --- R104 1k --- GPIO26/ADC0 --- C104 22nF --- GND
//	J103.S, J103.TN, J103.RN --- GND
//
// The 1k in series is what makes it safe to drive either pin as an
// output: everything a guitarist can plug in here shorts one of them to
// ground sooner or later, and 3.3mA is not a fault.  A BAT54S on each
// clamps to the rails.
//
// Both normalling contacts are grounded, so an *empty* jack reads as two
// pins held at zero - the same thing a closed footswitch reads as.  That
// is a real ambiguity and not an oversight of the wiring; see exp.h.
//
// These are the two pins the split boards spend on hardware MIDI, which
// is why the expression jack could not have existed on them.
//
#define EXP_TIP_GPIO		27
#define EXP_RING_GPIO		26
#define EXP_TIP_ADC		1
#define EXP_RING_ADC		0

//
// The smart LEDs.  Three WS2812B-2020 in a chain off one pin, driven
// straight from 3.3V logic - modern parts take 0.55*VSS as a high, and
// these do; older ones and clones want a level shifter.
//
// GPIO 25 is not a leftover: it is what this board wires them to, and
// the knob test board independently arrived at the same pin.
//
#define WS2812_GPIO		25
#define NR_LEDS			3
