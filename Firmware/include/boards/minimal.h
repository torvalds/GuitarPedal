//
// The minimal board.  Hardware/minimal.
//
// One board, no daughtercard, no split: an RP2354A, a stereo codec, one
// op-amp buffer, two audio jacks and a headphone jack.  No rotary, no
// MIDI jacks, no expression jack, no screen.  It exists to be the
// smallest thing that is still the pedal, and it is what gets used as a
// measurement instrument for other pedals.
//
// Two revisions share this map.  The earlier one has a TAC5242, which is
// hardware-strapped and AC-coupled through input capacitors; the later
// one has a TAC5212, which is set up over i2c and DC-coupled.  Neither is
// a build option: the firmware finds out which is in front of it by
// asking i2c, and having found out it says so.  See probe_hardware().
//

//
// No hardware MIDI - USB is the only way in or out.  No rotary encoder,
// no expression jack and no footswitch, so ROTARY_*_GPIO, EXP_*_GPIO
// and STOMP_GPIO are absent rather than zero: the code keys off whether
// they exist.
//
// GPIO10 is the radio's on this board.  The earlier revisions put a
// footswitch there, and no enclosure was ever built with a switch in
// it - at this size there is nowhere sensible to put one.
//
// No plain LED either.  D101 is on VBUS through a resistor, a power
// indicator the firmware cannot reach, so LED_GPIO is absent and the
// WS2812s below are the only LEDs there are.
//

//
// i2s, and the one place this board's pin order is not the obvious one.
//
// The codec's serial pins run 2, 3, 4, 5 = BCLK, FSYNC, DOUT, DIN, and
// it sits rotated against the MCU, so ascending GPIO meets descending
// codec pin.  That leaves FSYNC *below* BCLK, where every earlier board
// had BCLK below FSYNC.
//
// That is not a free relabelling.  BCLK and FSYNC are driven from one
// two-bit PIO side-set, and side-set bit 0 is whichever pin sits at the
// base - so the two orders need different values in the instructions
// themselves, which is what I2S_FSYNC_BELOW_BCLK selects.  See i2s.pio.
//
#define I2S_DIN			20
#define I2S_DOUT		21
#define I2S_FSYNC		22
#define I2S_BCLK		23
#define I2S_FSYNC_BELOW_BCLK	1

//
// i2c to the codec, and the only i2c on the board: no screen, no eeprom,
// so i2c1 is absent and its pins are not defined at all.
//
// ADDR shorted to ground gives 7-bit 0x50 (SLASF23A table 7-72), where
// the split boards' codec answers at 0x51.  A TAC5242 has no i2c to
// answer with, which is the distinction probe_hardware() is drawing.
//
#define I2C0_SDA		16
#define I2C0_SCL		17
#define TAC5112_I2C		i2c0, 0x50

//
// What an answer on i2c means *here*, which is not what it means on the
// split boards.  There it tells a mono audio board from a stereo one;
// here both revisions are stereo and it tells a DC-coupled input from an
// AC-coupled one - no low-frequency corner against a corner at 10Hz.
// That is the difference that matters when this board is the instrument
// rather than the thing being measured.
//
#define CODEC_I2C_DESC		"DC-coupled"
#define CODEC_STRAPPED_DESC	"AC-coupled"

//
// ...and what to set it up as, when it is the one that answers.  The
// input has no capacitors into the codec and the headphone jack shorts
// OUT1M to OUT2M, so both halves of the analog configuration differ from
// every earlier board.  See tac5112.h.
//
#define CODEC_DC_COUPLED	1

//
// The nRF54L10, and the eleven wires to it.
//
// Nothing on the board names a role: the schematic labels these nets by
// their nRF54 pin names and the PCB nets come out as /RP2354/GPIOn.  So
// what each wire carries is decided here and nowhere else, and the nRF
// pin it lands on is in the comment because two of the choices are
// forced by it.
//
#define NRF54_SWDCLK		2
#define NRF54_SWDIO		3
#define NRF54_RESET		12	// active low

//
// The command link, which carries MIDI and nothing else.
//
// UART1 on this side, free because this board has no MIDI jacks, and
// function select 2 is what makes these four pins that UART.  UARTE30
// on the nRF, because all four are in its low-power domain and a
// peripheral there cannot reach across ports.
//
// Which pin is which is ours to pick on both sides - the nRF chooses
// each signal's pin in a register - so P0.01 is its receive against our
// transmit.
//
#define NRF54_UART		uart1
#define NRF54_TX		4	// nRF P0.01, its RXD
#define NRF54_RX		5	// nRF P0.00, its TXD
#define NRF54_CTS		6	// nRF P0.02, its RTS
#define NRF54_RTS		7	// nRF P0.03, its CTS
#define NRF54_UART_FUNCSEL	2

//
// The audio link, which nothing drives yet.
//
// Two constraints meet on these four pins and one arrangement satisfies
// both.  The nRF needs its i2s clock on one of the pins that can carry
// a clock, and of the four only P1.03 and P1.04 can; the PIO drives
// BCLK and FSYNC from one two-bit field, so they must be adjacent and
// in the I2S_FSYNC_BELOW_BCLK order above.  Taking P1.04 for BCLK also
// keeps the clock off P1.02 and P1.03, which are the NFC antenna pins
// and are GPIOs only once the radio's firmware says so.
//
// DIN and DOUT are named from the far end, the way the codec's are, so
// DIN is an MCU output.
//
#define NRF54_I2S_FSYNC		8	// nRF P1.05
#define NRF54_I2S_BCLK		9	// nRF P1.04, a clock pin
#define NRF54_I2S_DIN		10	// nRF P1.03
#define NRF54_I2S_DOUT		11	// nRF P1.02

//
// Two WS2812B-4020s off one pin, side-emitting.  Driven straight from
// 3.3V logic. No level shifter needed, the WS2812B considers anything
// above ~2.7V as a high and can be driven by 3.3V logic (depending on
// datasheet it either says "2.7V" explicitly, or says "0.55*VDD").
//
#define WS2812_GPIO		1
#define NR_LEDS			2
