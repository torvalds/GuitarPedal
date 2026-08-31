#ifndef EXP_H
#define EXP_H

//
// The expression jack, as a probe rather than as a feature.
//
// Nothing here is wired into the pedal.  It exists to answer "what is
// actually plugged into that jack, and can we read it well enough to be
// worth having" on a board where the jack has never been used, and it is
// driven entirely by a SysEx request from the host.  When the answer is
// known this wants replacing with something that polls, debounces and
// maps to a pot; until then, guessing at that design would be guessing.
//
// WHAT IS OUT THERE.  Two things, and they want opposite pin setups:
//
//   - a double footswitch, tip and ring each shorting to sleeve.  Wants
//     a pull-up on both pins and a threshold.
//   - an expression pedal, which is a potentiometer on a TRS plug.  Wants
//     one pin driven as the supply and the other read as the wiper.
//
// Expression pedals do not agree on which contact is the wiper.  The
// Roland/Boss convention is ring = supply, tip = wiper; others are the
// other way about.  So this probes *both* polarities rather than picking
// one, and whatever uses it later can decide from the numbers instead of
// from a datasheet nobody has.
//
// WHICH POLARITY, AND WHY IT IS NOT A COIN TOSS.  Measured against a
// Sonicake VEXPRESS, which is Roland-convention - tip = wiper, ring =
// supply, sleeve = the bottom of a nominal 10k pot:
//
//	drive RING -> read TIP      3 .. 3677    span 3674
//	drive TIP  -> read RING      6 .. 3676    span 3670
//
// Both sweep nearly the full range, so "does it move" picks neither.
// The difference is the shape, and it comes straight out of which part
// of the pot the series resistor is loaded by:
//
//	drive the top, read the wiper:   V = 3.3 x Rp / (Rs + Rp)
//	drive the wiper, read the top:   V = 3.3 x Rp / (Rs + x Rp)
//
// The first is linear in x because the whole pot loads Rs whatever the
// treadle is doing.  The second has only the lower section loading it,
// and that section shrinks as the reading rises - so it saturates, and
// at half travel it is already at 92% of its range.  Drive the supply
// and read the wiper; the other way round works and is useless.
//
// WHAT THE READINGS MEAN.  Full scale is 4095 = 3.3V.  With nothing in
// the jack the normalling contacts ground both pins, so every reading
// but the temperature is near zero - which is also what a footswitch
// held down looks like, and the reason this cannot tell an empty jack
// from two closed switches.  A plug lifts the normalling contact, so
// "plugged in and not pressed" is the case that reads high, and that
// asymmetry is the only handle there is.
//
// THE PULL-UP IS SLOW.  The internal pull-up is around 56k and there is
// 22nF at the pin, so it charges with a 1.2ms time constant while the
// discharge through the 1k is 22us.  A press is seen almost at once and
// a release takes milliseconds.  Fine for a foot, worth knowing before
// anyone reads these numbers as instantaneous.
//
// THIS BLOCKS CORE 0 for about 20ms, nearly all of it waiting for that
// capacitor.  Acceptable for something a host asks for by hand and not
// acceptable for anything periodic, which is the other reason the real
// version cannot just call this in a loop.
//
#include "hardware/adc.h"

#ifdef EXP_TIP_GPIO

//
// If this ever fires, the build has been pointed at an RP2350B: the ADC
// moves to GPIO40-47 there and these two pin numbers become somebody
// else's.
//
// The part is an RP2354A - QFN-60, GPIO0-29, ADC on 26-29 - and nothing
// in the build says so directly: PICO_BOARD is left unset, the SDK
// defaults it to pico2, and pico2.h is what defines PICO_RP2350A.  So
// the pin numbers above rest on a default three files away, and setting
// PICO_BOARD to a B-part board would move them without touching
// anything that looks like it is about pins.
//
// Worth an assert rather than a comment because the paperwork had it
// wrong for a while: CLAUDE.md and CMakeLists.txt both called the part
// an RP2354B, against a schematic symbol that says RP2354A.  Fixed now,
// but the way that mistake would have come back is somebody making the
// build agree with the wrong half.
//
_Static_assert(ADC_BASE_PIN == 26, "the ADC has moved; EXP_*_GPIO are wrong");

#define EXP_SETTLE_PULLUP_MS	12	// ~10 time constants of 56k x 22nF
#define EXP_SETTLE_DRIVEN_MS	4	// 1k plus a pot, into the same 22nF
#define EXP_READINGS		8

//
// Read one channel, having given the mux time to settle.
//
// Averaged over eight, which is not filtering so much as refusing to
// report a single conversion as if it were a measurement: the question
// being asked is how many levels this jack is good for, and one sample
// answers it pessimistically for no reason.
//
static uint16_t exp_read(int channel)
{
	unsigned int sum = 0;

	adc_select_input(channel);
	for (int i = 0; i < EXP_READINGS; i++)
		sum += adc_read();
	return (sum + EXP_READINGS / 2) / EXP_READINGS;
}

// Back to the state everything else here assumes: hi-Z, no pulls.
static void exp_idle(void)
{
	adc_gpio_init(EXP_TIP_GPIO);
	adc_gpio_init(EXP_RING_GPIO);
}

//
// Both pins as analog inputs with the pull-ups on.  The footswitch case,
// and the one that says whether anything is shorting to sleeve.
//
static void exp_pullups(void)
{
	exp_idle();
	gpio_pull_up(EXP_TIP_GPIO);
	gpio_pull_up(EXP_RING_GPIO);
	sleep_ms(EXP_SETTLE_PULLUP_MS);
}

//
// Empty the 22nF at a pin before anything tries to read it.
//
// There is no discharge path through an open jack, so without this the
// reading is whatever the *previous* step left on the capacitor - and
// the step before happens to be the pull-up, which charges it to the
// rail.  Measured on an unplugged-at-the-far-end TRS cable, the driven
// rows came back 4041 and 4047: an open circuit reported as an
// expression pedal held at maximum, which is the one wrong answer that
// would have been believed.
//
// Held low as an output rather than pulled down, because 50 ohms empties
// it immediately and a pull-down would take another time constant.  What
// the reading then shows is what the outside world puts *back*, so an
// open circuit stays near zero and a pot climbs to its wiper.
//
static void exp_drain(unsigned int gpio)
{
	gpio_init(gpio);
	gpio_set_dir(gpio, GPIO_OUT);
	gpio_put(gpio, 0);
	sleep_ms(1);
}

//
// One pin driven high as the supply, the other read as the wiper.
//
// adc_gpio_init() on the pin being read, gpio_init() on the pin being
// driven: the first selects the null function so the output driver is
// hi-Z and the analog side sees the pad, the second puts SIO back in
// charge of it.  Getting these the wrong way round reads the pin that is
// driving, which is a very convincing 4095.
//
static void exp_drive(unsigned int drive_gpio, unsigned int read_gpio)
{
	exp_idle();
	gpio_disable_pulls(drive_gpio);
	gpio_disable_pulls(read_gpio);

	exp_drain(read_gpio);
	adc_gpio_init(read_gpio);
	gpio_init(drive_gpio);
	gpio_set_dir(drive_gpio, GPIO_OUT);
	gpio_put(drive_gpio, 1);
	sleep_ms(EXP_SETTLE_DRIVEN_MS);
}

//
// Every configuration worth having, in one sweep.
//
// Filled in the order the reply documents, because the host end is a
// list of names and this is the only thing that says which is which.
//
enum {
	EXP_FLOAT_RING, EXP_FLOAT_TIP,		// hi-Z: residual, see below
	EXP_PULLUP_RING, EXP_PULLUP_TIP,	// footswitches
	EXP_DRIVETIP_RING,			// tip = supply, ring = wiper
	EXP_DRIVERING_TIP,			// ring = supply, tip = wiper
	EXP_TEMPERATURE,			// the ADC proving it works at all
	EXP_NR_READINGS
};

//
// The float pair is the one reading here that is not a measurement.
//
// A hi-Z pin with 22nF on it holds whatever charge it was left with, and
// with an open jack nothing takes it away, so what comes back is history
// rather than state.  It is kept because the two ends of it are still
// worth telling apart - an empty jack is held at zero by the normalling
// contacts and reads 3, while a plugged-in cable floats and reads
// somewhere near mid rail - but the pull-up pair answers the same
// question by actually driving the pin, and that is the one to believe.
//
static void init_exp_switches(void);

static void exp_probe(uint16_t out[EXP_NR_READINGS])
{
	exp_idle();
	sleep_ms(1);
	out[EXP_FLOAT_RING] = exp_read(EXP_RING_ADC);
	out[EXP_FLOAT_TIP] = exp_read(EXP_TIP_ADC);

	exp_pullups();
	out[EXP_PULLUP_RING] = exp_read(EXP_RING_ADC);
	out[EXP_PULLUP_TIP] = exp_read(EXP_TIP_ADC);

	exp_drive(EXP_TIP_GPIO, EXP_RING_GPIO);
	out[EXP_DRIVETIP_RING] = exp_read(EXP_RING_ADC);

	exp_drive(EXP_RING_GPIO, EXP_TIP_GPIO);
	out[EXP_DRIVERING_TIP] = exp_read(EXP_TIP_ADC);

	//
	// Nothing to do with the jack.  If the two pins above read zero it
	// is worth knowing whether that is the wiring or the converter, and
	// the temperature sensor is the one input on this chip whose answer
	// is known in advance: room temperature lands near 0.7V, which is
	// about 890 counts.  A dead ADC reads 0 or 4095 here too.
	//
	adc_set_temp_sensor_enabled(true);
	sleep_ms(1);
	out[EXP_TEMPERATURE] = exp_read(ADC_TEMPERATURE_CHANNEL_NUM);
	adc_set_temp_sensor_enabled(false);

	exp_idle();

	// Every step above took the pins for itself, so hand them back to
	// whatever was using them.
	init_exp_switches();
}

//
// What the readings add up to.
//
// The probe answers in numbers because when it was written nobody knew
// what to expect from them.  They have been seen now, so the pedal says
// what it thinks is out there rather than leaving the host to keep a
// second copy of the thresholds - and the pedal is the end that has to
// act on the answer.
//
enum exp_accessory {
	EXP_ACC_NONE,		// an empty jack, grounded by its own normalling
	EXP_ACC_SWITCHES,	// footswitches shorting tip or ring to sleeve
	EXP_ACC_TREADLE,	// a potentiometer on a TRS plug
	EXP_ACC_UNKNOWN,	// something else, which is worth saying plainly
};

//
// Below EXP_LOW a pin is at the bottom of its range - grounded, or a
// wiper at its heel stop.  Above EXP_HIGH a pull-up reached the rail,
// so nothing is shorting that pin to sleeve.
//
// Measured on unified/8736, pull-up ring/tip then the two driven rows:
//
//	empty jack		  69	  69	   0	   0
//	dual-stomp, none down	4072	4070	  52	  57
//	treadle, heel		 812	  69	   1	   0
//	treadle, toe		1392	1392	3674	3674
//
// Every state clears the nearer threshold by more than 2.5x.  An empty
// jack reads 69 rather than 0 because the pull-up drives through the
// 1k series resistor into the normalling contact, which is the divider
// the schematic predicts.
//
#define EXP_LOW		256
#define EXP_HIGH	3500

static int exp_classify(const uint16_t r[EXP_NR_READINGS])
{
	//
	// A pot is the only thing out there that feeds a driven pin's
	// voltage back to the other one, so this is the single positive
	// test and it comes first.  A treadle parked at its heel stop
	// feeds back nothing, which is why moving it is part of being
	// found.
	//
	if (r[EXP_DRIVERING_TIP] >= EXP_LOW || r[EXP_DRIVETIP_RING] >= EXP_LOW)
		return EXP_ACC_TREADLE;

	//
	// One pin reaching the rail means a plug is in: an empty jack
	// grounds both through its normalling contacts.  One is enough,
	// because the other may be held down by a switch.
	//
	if (r[EXP_PULLUP_TIP] >= EXP_HIGH || r[EXP_PULLUP_RING] >= EXP_HIGH)
		return EXP_ACC_SWITCHES;

	if (r[EXP_PULLUP_TIP] < EXP_LOW && r[EXP_PULLUP_RING] < EXP_LOW)
		return EXP_ACC_NONE;

	return EXP_ACC_UNKNOWN;
}

//
// The Exp Jack pot in settings.h stores one of these, so it has to list
// exactly the ones that can be stored - every accessory but UNKNOWN,
// which is something the probe reports and never something you set.
//
_Static_assert(ARRAY_SIZE(settings_exp_jack_enum) == EXP_ACC_UNKNOWN + 1,
	       "the Exp Jack pot and enum exp_accessory have drifted apart");

//
// The jack's two pins as switches, which they are only while the
// setting says an accessory with switches on it is plugged in: a
// treadle wants them for the ADC, and an empty jack grounds them, which
// would read as held down for ever.
//
// Wants the settings loaded, so it runs after init_effects() rather
// than with the rest of the bring-up - which means changing the setting
// takes effect at the next boot.
//
static void init_exp_switches(void)
{
	if (settings.exp_jack != EXP_ACC_SWITCHES)
		return;

	for (int sw = NR_ONBOARD_SWITCHES; sw < NR_SWITCHES; sw++) {
		init_sw_pin(pio1, switch_gpio[sw]);
		debounce_program_init(pio1, sw, debounce_offset, switch_gpio[sw]);
	}
}

static void exp_init(void)
{
	adc_init();
	exp_idle();
}

#endif	// EXP_TIP_GPIO
#endif
