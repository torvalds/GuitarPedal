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
// No hardware MIDI - USB is the only way in or out.  No rotary encoder
// and no expression jack, so ROTARY_*_GPIO and EXP_*_GPIO are absent
// rather than zero: the code keys off whether they exist.
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
// The footswitch is a magnet on a spring rather than a contact.  Pressing
// it carries the magnet *through* the board past U101, which latches the
// sensor on; the spring carries it back through the other way, which
// latches it off.  So the pin is the state of the switch, its two edges
// are press and release, and it behaves as the momentary switch it
// replaces.
//
// Which makes it a switch like any other, so it goes through the same
// debounce program - and that is also where a press is timed: a stable low for a
// second is a hold and a release before that is a tap.  It needs no
// debouncing, and being debounced anyway costs nothing but the state
// machine every other switch already spends - and it keeps this board
// identical to the ones with a real contact on them.  Issue 380 holds
// the simpler thing to do instead, if every board ever goes magnetic.
//
// Pressed reads low, the same as a contact to ground.
#define STOMP_GPIO		10

//
// Two WS2812B-4020s off one pin, side-emitting.  Driven straight from
// 3.3V logic. No level shifter needed, the WS2812B considers anything
// above ~2.7V as a high and can be driven by 3.3V logic (depending on
// datasheet it either says "2.7V" explicitly, or says "0.55*VDD").
//
#define WS2812_GPIO		1
#define NR_LEDS			2
