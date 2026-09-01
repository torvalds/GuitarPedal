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
// A SWEEP TAKES ABOUT 24ms, nearly all of it waiting for that capacitor,
// and it blocks nothing: exp_sweep_task() takes one step per call from
// the main loop.  Which is also what a treadle will need, for the
// opposite reason - it stops at the driven step and stays there.
//
#include "hardware/adc.h"

//
// Defined in midi/sysex.h, which comes after this in pedal.c's include
// list - it has to, because it reports what this decides.  Declared here
// rather than there so that usb-device.c, which is the other translation
// unit and includes midi/midi.h, is not told about a static function it
// will never see a definition of.
//
// Learning a treadle's travel is the one thing on this side of that line
// with something to say to the host while it is happening.
//
static bool sysex_echo_pots(int eff, const uint8_t *pairs, int n);

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
}

//
// And the same in the other direction, before handing a pin to a switch.
//
// The pull-up that holds an idle switch high is the chip's own 56k, and
// there is 22nF at the jack, so a pin released to it climbs for about
// twelve milliseconds - EXP_SETTLE_PULLUP_MS is that number, and the
// sweep waits it out for its own readings.  debounce.pio does not: it
// opens with 'wait 0 jmppin', a *level*, so a state machine started on a
// pin still on its way up decides the switch is held and reports a press
// when the capacitor finally arrives.
//
// Which is one invented press per probe, on the tip, because the sweep's
// last driven step leaves the ring charged and the tip drained.
//
// 50 ohms gets there in microseconds where 56k takes twelve
// milliseconds, and 200us is a couple of hundred time constants of it.
// It cannot hide a real press: a switch that is genuinely held pulls the
// pin back down through its own 1k in about 22us, long before the
// program's first sample a millisecond later.
//
static void exp_precharge(unsigned int gpio)
{
	gpio_init(gpio);
	gpio_set_dir(gpio, GPIO_OUT);
	gpio_put(gpio, 1);
	busy_wait_us(200);
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
static void exp_drive_begin(unsigned int drive_gpio, unsigned int read_gpio)
{
	exp_idle();
	gpio_disable_pulls(drive_gpio);
	gpio_disable_pulls(read_gpio);
	exp_drain(read_gpio);
}

static void exp_drive_end(unsigned int drive_gpio, unsigned int read_gpio)
{
	adc_gpio_init(read_gpio);
	gpio_init(drive_gpio);
	gpio_set_dir(drive_gpio, GPIO_OUT);
	gpio_put(drive_gpio, 1);
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
static void init_exp_pins(void);


//
// The sweep, as a task rather than a blocking call.
//
// Every step has the same shape: put the pins in some state, wait for
// the 22nF to settle, read.  The waiting is nearly all of the 24ms and
// nothing needs the processor during it, so the main loop calls in and
// each call either finds the deadline has not come or takes one step.
//
// The treadle will want this machinery for the opposite reason: it stops
// at the driven step and reads that over and over instead of moving on.
//
enum {
	EXP_SWEEP_IDLE = -2,	// nothing to do
	EXP_SWEEP_DONE = -1,	// readings waiting to be picked up
};

static struct {
	int step;
	absolute_time_t due;
	uint16_t out[EXP_NR_READINGS];
} sweep = { .step = EXP_SWEEP_IDLE };

// Ignored while one is running or while a result has not been read.
static void exp_sweep_start(void)
{
	if (sweep.step != EXP_SWEEP_IDLE)
		return;
	sweep.step = 0;
	sweep.due = get_absolute_time();
}

static bool exp_sweep_ready(void)
{
	return sweep.step == EXP_SWEEP_DONE;
}

static bool exp_sweep_busy(void)
{
	return sweep.step >= 0;
}

static const uint16_t *exp_sweep_take(void)
{
	sweep.step = EXP_SWEEP_IDLE;
	return sweep.out;
}

static void exp_next(unsigned int ms)
{
	sweep.due = delayed_by_ms(get_absolute_time(), ms);
	sweep.step++;
}

static void exp_sweep_task(void)
{
	if (sweep.step < 0)
		return;
	if (absolute_time_diff_us(get_absolute_time(), sweep.due) > 0)
		return;

	switch (sweep.step) {
	case 0:
		exp_idle();
		exp_next(1);
		break;
	case 1:
		sweep.out[EXP_FLOAT_RING] = exp_read(EXP_RING_ADC);
		sweep.out[EXP_FLOAT_TIP] = exp_read(EXP_TIP_ADC);
		//
		// Empty both before pulling them up.  A pull-up can only
		// pull up, so otherwise the reading starts from wherever
		// the last thing to touch the pin left the capacitor -
		// this sweep's own driven step and the accessory LED's
		// PWM both leave it at the rail, and getting down from
		// there is a discharge the pull-up opposes.  From zero
		// it is charging, which is the one direction it can do.
		//
		exp_drain(EXP_RING_GPIO);
		exp_drain(EXP_TIP_GPIO);
		exp_next(1);
		break;
	case 2:
		exp_pullups();
		exp_next(EXP_SETTLE_PULLUP_MS);
		break;
	case 3:
		sweep.out[EXP_PULLUP_RING] = exp_read(EXP_RING_ADC);
		sweep.out[EXP_PULLUP_TIP] = exp_read(EXP_TIP_ADC);
		exp_drive_begin(EXP_TIP_GPIO, EXP_RING_GPIO);
		exp_next(1);
		break;
	case 4:
		exp_drive_end(EXP_TIP_GPIO, EXP_RING_GPIO);
		exp_next(EXP_SETTLE_DRIVEN_MS);
		break;
	case 5:
		sweep.out[EXP_DRIVETIP_RING] = exp_read(EXP_RING_ADC);
		exp_drive_begin(EXP_RING_GPIO, EXP_TIP_GPIO);
		exp_next(1);
		break;
	case 6:
		exp_drive_end(EXP_RING_GPIO, EXP_TIP_GPIO);
		exp_next(EXP_SETTLE_DRIVEN_MS);
		break;
	case 7:
		sweep.out[EXP_DRIVERING_TIP] = exp_read(EXP_TIP_ADC);
		//
		// Nothing to do with the jack.  If the two pins above
		// read zero it is worth knowing whether that is the
		// wiring or the converter, and the temperature sensor is
		// the one input on this chip whose answer is known in
		// advance: room temperature lands near 0.7V, which is
		// about 890 counts.  A dead ADC reads 0 or 4095 here too.
		//
		adc_set_temp_sensor_enabled(true);
		exp_next(1);
		break;
	default:
		sweep.out[EXP_TEMPERATURE] = exp_read(ADC_TEMPERATURE_CHANNEL_NUM);
		adc_set_temp_sensor_enabled(false);
		exp_idle();

		// Every step took the pins for itself, so hand them back
		// to whatever was using them.
		init_exp_pins();
		sweep.step = EXP_SWEEP_DONE;
		break;
	}
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
//
// The Detect pot, whose values are the pedal's own business rather than
// something to compare a reading against.
//
enum {
	EXP_DETECT_MANUAL,
	EXP_DETECT_AUTO,
};

enum exp_accessory {
	EXP_ACC_NONE,		// an empty jack, grounded by its own normalling
	EXP_ACC_SWITCHES,	// footswitches shorting tip or ring to sleeve
	EXP_ACC_TREADLE,	// a potentiometer on a TRS plug
	EXP_ACC_STOMP_LED,	// one switch on the tip, an LED on the ring
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
//	single-stomp + LED	2693	4070	  47	  57
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

	bool tip_open = r[EXP_PULLUP_TIP] >= EXP_HIGH;
	bool ring_open = r[EXP_PULLUP_RING] >= EXP_HIGH;

	//
	// A plug whose ring reaches neither rail has something on it that
	// is not a switch, and an LED is the one such thing that exists:
	// it clamps the pull-up at its forward drop.  Worth telling apart
	// from a pair of switches, because that ring is an output and
	// sits below VIH - read as a switch it would be held down for
	// ever.
	//
	if (tip_open && !ring_open && r[EXP_PULLUP_RING] >= EXP_LOW)
		return EXP_ACC_STOMP_LED;

	//
	// One pin reaching the rail means a plug is in: an empty jack
	// grounds both through its normalling contacts.  One is enough,
	// because the other may be held down by a switch.
	//
	// A TRS cable with nothing on the far end lands here too, and
	// that is right rather than a miss: an open tip and an open ring
	// is exactly what a switch box with nothing pressed looks like.
	// What makes an *empty jack* knowable is the normalling contacts,
	// and a cable lifts them, so no probe can separate the two.
	//
	if (tip_open || ring_open)
		return EXP_ACC_SWITCHES;

	if (r[EXP_PULLUP_TIP] < EXP_LOW && r[EXP_PULLUP_RING] < EXP_LOW)
		return EXP_ACC_NONE;

	return EXP_ACC_UNKNOWN;
}

//
// Is what is out there still consistent with what somebody said it was?
//
// A different question from exp_classify(), and a much easier one.
// Naming a jack from a reading means picking one of five, and the pot
// that gives the only positive test goes quiet at its heel stop.
// Confirming a name means ruling it out, and a reading no rule can name
// is usually still perfectly consistent with something: 812/69 is what a
// heel-down treadle looks like, and nothing else on the list looks like
// it except a stomp whose switch happens to be held at that moment.
//
// Asymmetric on purpose.  This can confirm a wrong answer somebody
// typed; it cannot invent one.  To make the pedal start over, set the
// accessory to Nothing - which is also the only state anything automatic
// discovers from.
//
static bool exp_verify(const uint16_t r[EXP_NR_READINGS], int accessory)
{
	bool tip_open = r[EXP_PULLUP_TIP] >= EXP_HIGH;
	bool ring_open = r[EXP_PULLUP_RING] >= EXP_HIGH;
	bool tip_low = r[EXP_PULLUP_TIP] < EXP_LOW;
	bool ring_low = r[EXP_PULLUP_RING] < EXP_LOW;

	switch (accessory) {
	case EXP_ACC_NONE:
		//
		// The normalling contacts and nothing else.  A plug lifts
		// them, so anything off the bottom is a plug - but a
		// switch box with everything held down reads the same,
		// and nothing here can say otherwise.
		//
		return tip_low && ring_low;

	case EXP_ACC_SWITCHES:
		//
		// Each pin is a switch: open, or shorted to sleeve.  A pin
		// resting anywhere between the two has something on it
		// that is not a switch.
		//
		return (tip_open || tip_low) && (ring_open || ring_low);

	case EXP_ACC_STOMP_LED:
		//
		// The LED clamps the ring's pull-up at its forward drop,
		// which is the whole of the evidence and is there whether
		// or not the switch is down.
		//
		return !ring_open && !ring_low && (tip_open || tip_low);

	case EXP_ACC_TREADLE:
		//
		// A wiper feeding a driven pin's voltage back is proof,
		// and the only proof there is - but it is gone at the heel
		// stop, where the wiper sits at sleeve and feeds nothing.
		// What is left there is the supply pin, still a divider
		// against the pull-up and so reading neither rail while
		// the wiper reads the bottom.
		//
		// Either pin may be either: the two conventions disagree
		// about which, and this does not have to care.
		//
		if (r[EXP_DRIVERING_TIP] >= EXP_LOW ||
		    r[EXP_DRIVETIP_RING] >= EXP_LOW)
			return true;
		return (tip_low && !ring_low && !ring_open) ||
		       (ring_low && !tip_low && !tip_open);
	}
	return false;
}

//
// What to call what is out there, given what has already been said about
// it.  Confirming beats guessing, and the cases where they differ are
// exactly the readings where guessing has nothing to go on.
//
static int exp_verdict(const uint16_t r[EXP_NR_READINGS])
{
	if (exp_verify(r, expression.accessory))
		return expression.accessory;
	return exp_classify(r);
}

//
// The Accessory pot in expression.h stores one of these, so it has to list
// exactly the ones that can be stored - every accessory but UNKNOWN,
// which is something the probe reports and never something you set.
//
_Static_assert(ARRAY_SIZE(expjack_accessory_enum) == EXP_ACC_UNKNOWN + 1,
	       "the Accessory pot and enum exp_accessory have drifted apart");

// The same, for Detect - which has no "and something else" value, so the
// names and the enum are simply the same length.
_Static_assert(ARRAY_SIZE(expjack_detect_enum) - 1 == EXP_DETECT_AUTO + 1,
	       "the Detect pot and its enum have drifted apart");

//
// The jack's pins, handed to whatever the setting says is on them.
//
// They are only switches while something says they are: the same two
// pins are a treadle's wiper and supply, and an empty jack grounds them,
// either of which reads as held down for ever.
//
// Called once the settings have loaded, whenever the setting moves after
// that, and again after every sweep - each step of a sweep takes both
// pins for itself.
//
static void init_exp_switch(int sw)
{
	exp_precharge(switch_gpio[sw]);
	init_sw_pin(pio1, switch_gpio[sw]);
	debounce_program_init(pio1, sw, debounce_offset, switch_gpio[sw]);
}

//
// Full brightness, and no setting for it.  194 fixes the current at
// (3.3 - Vf)/1k on a board that is already made, so there is not much
// of it to spend and dimming is not what is short.
//
static bool exp_led_lit;

static void exp_led_set(bool on)
{
	if (expression.accessory != EXP_ACC_STOMP_LED)
		return;
	exp_led_lit = on;
	pwm_set_gpio_level(EXP_RING_GPIO, on ? PWM_WRAP : 0);
}

//
// The same again, for something that borrowed the pin.
//
// init_one_pwm_pin() sets the level to zero, and update_ui() would put it
// back on its next tick - but a lamp that blinks whenever somebody holds
// the footswitch is not a thing to leave to the timing of another task.
//
static void exp_led_restore(void)
{
	init_one_pwm_pin(EXP_RING_GPIO);
	pwm_set_gpio_level(EXP_RING_GPIO, exp_led_lit ? PWM_WRAP : 0);
}

//
// Which pin is which, for a treadle.  Roland and Boss put the wiper on
// the tip and drive the ring; Yamaha and Korg are the other way about.
//
static unsigned int exp_wiper_gpio(void)
{
	return expression.type ? EXP_RING_GPIO : EXP_TIP_GPIO;
}

static unsigned int exp_supply_gpio(void)
{
	return expression.type ? EXP_TIP_GPIO : EXP_RING_GPIO;
}

static int exp_wiper_adc(void)
{
	return expression.type ? EXP_RING_ADC : EXP_TIP_ADC;
}

//
// Where the treadle is, read where it stands rather than swept for.
//
// The pins park driven, so nothing is being reconfigured between
// samples and nothing has to settle: the only filter left is the 22nF,
// which against the worst case of a 100k pot at mid travel is about
// 570us.  A foot takes tens of milliseconds, so the capacitor is not
// what decides how quickly this follows - the sampling rate is, which
// is why it runs from the main loop rather than the 25Hz tick.
//
static uint16_t exp_treadle_raw;

static void exp_treadle_task(void)
{
	if (expression.accessory != EXP_ACC_TREADLE)
		return;
	if (exp_sweep_busy())
		return;		// the sweep has the pins
	exp_treadle_raw = exp_read(exp_wiper_adc());
}

//
// Learning the treadle's travel, and only while asked to.
//
// A new end has to be seen twice running before it is taken, and the
// less extreme of the two is what is kept, so a single stray reading
// can never set one.  Cheap, and the window is short enough that the
// real protection is the window itself.
//
// Closing on a range too small to be a sweep puts full scale back
// rather than keeping something unusable - that is what "you did not
// actually rock it" looks like, and it should not leave the treadle
// driving a tenth of a pot.
//
#define TREADLE_MIN_SPAN 1000

//
// And how a sweep says it is finished, so that nobody has to.
//
// Not a timer and not a count of swings, but a swing that taught it
// nothing: reaching one end, then the other, then the first again with
// the range unmoved throughout.  Somebody who rocks a treadle slowly and
// carefully is still learning something on every pass and the window
// stays open for as long as that is true, which a stopwatch could not
// manage.
//
// It cannot tell "this treadle's travel is short" from "you did not push
// it far enough", and nothing can - the ends of a pot are wherever the
// mechanism stops. TREADLE_MIN_SPAN is the whole of the defence, the
// same as it is for a window closed by hand.
//
// An end is the outer eighth of what has been seen so far, which moves
// as the range grows and is meaningless until it is real - hence the
// span test inside exp_treadle_end() rather than around it.
//
#define TREADLE_SETTLED	3

static int exp_treadle_end(int raw, int lo, int hi)
{
	int span = hi - lo;

	if (span < TREADLE_MIN_SPAN)
		return 0;
	if (raw <= lo + span / 8)
		return -1;
	if (raw >= hi - span / 8)
		return 1;
	return 0;
}

//
// A reading back to a pot setting, asked of the same accessor that
// turns a pot setting into one - so the inverse follows the declared
// range instead of repeating it, which is how the two would come to
// disagree.
//
static unsigned char raw_to_pot(int raw, unsigned int idx)
{
	unsigned char probe[10] = { 0 };
	float pct = raw * (100.0f / 4095.0f);
	float lo, hi;
	int v;

	probe[idx] = 0;
	lo = idx == EXPJACK_HEEL ? expjack_heel_pot(probe) : expjack_toe_pot(probe);
	probe[idx] = 120;
	hi = idx == EXPJACK_HEEL ? expjack_heel_pot(probe) : expjack_toe_pot(probe);

	v = lrintf((pct - lo) * 120.0f / (hi - lo));
	return v < 0 ? 0 : v > 120 ? 120 : v;
}

//
// Saying, as it goes, what it has learned.
//
// One place that tells the app, whatever the reason:
// 'want' is the pair the app should be showing and 'shown' is the pair
// it has been told, so a message the queue refused is simply still
// pending next time round.  Nothing here has to be reliable on its own.
//
// The pair goes as one message rather than two calls.  Two would be one
// endpoint arriving and the other never doing: midi_tx_busy() means the
// queue is not empty, so the first echo fills it and the second is
// dropped - every time, and always the same one.
//
static void exp_calibrate_task(void)
{
	static bool learning;
	static int prev, lo, hi;
	static unsigned char want_lo = 0xff, want_hi = 0xff;
	static unsigned char shown_lo = 0xff, shown_hi = 0xff;
	static int at_end, quiet_ends;
	static bool say_closed;
	int raw = exp_treadle_raw;

	if ((bool)expression.learning != learning) {
		learning = expression.learning;
		if (learning) {
			lo = 4095;
			hi = 0;
			at_end = 0;
			quiet_ends = 0;

			//
			// Forget what the app has been told, so that
			// whatever this window ends up committing is
			// sent even if it lands back on the old value.
			//
			shown_lo = 0xff;
			shown_hi = 0xff;
		} else {
			//
			// Committed to the pots, so it is stored, drawn
			// and editable by hand - and written once here
			// rather than per sample while sweeping.
			//
			// TREADLE_MIN_SPAN above is what "you did not
			// actually rock it" looks like.
			//
			bool swept = hi - lo >= TREADLE_MIN_SPAN;

			want_lo = swept ? raw_to_pot(lo, EXPJACK_HEEL) : 0;
			want_hi = swept ? raw_to_pot(hi, EXPJACK_TOE) : 120;

			set_effect_pot(&expjack_effect, EXPJACK_HEEL, want_lo);
			set_effect_pot(&expjack_effect, EXPJACK_TOE, want_hi);
		}
		prev = raw;
	}

	//
	// What it has learned so far, told to the app but not written to
	// the pots.
	//
	// Told, because a window somebody has to close by hand is a
	// window they have to be able to see working - rocking a treadle
	// at a screen that shows nothing looks exactly like a jack that
	// is not reading.
	//
	// Not written, because the stored pair is what the pedal falls
	// back on: power lost half way through a sweep should leave the
	// last calibration that worked rather than half of one.  It also
	// keeps core 1 out of it - every set_effect_pot() is another
	// expjack_init() over there, and a sweep would be hundreds.
	//
	if (learning && expression.accessory == EXP_ACC_TREADLE) {
		unsigned char was_lo = want_lo, was_hi = want_hi;
		int end;

		if (raw < lo && prev < lo)
			lo = raw > prev ? raw : prev;
		if (raw > hi && prev > hi)
			hi = raw < prev ? raw : prev;
		prev = raw;

		want_lo = raw_to_pot(lo, EXPJACK_HEEL);
		want_hi = raw_to_pot(hi, EXPJACK_TOE);

		//
		// Anything learned starts the count again, so the ends
		// only add up once the range has stopped moving.
		//
		// Measured in pot steps rather than converter counts,
		// which is not a detail: a treadle held against its stop
		// still dithers by a count or so, and "seen twice
		// running" lets that widen the range a count at a time.
		// A step is about thirty-four counts, so the setting has
		// to actually change before this calls it learning -
		// and it is the setting, not the reading, that the whole
		// window exists to arrive at.
		//
		if (want_lo != was_lo || want_hi != was_hi) {
			at_end = 0;
			quiet_ends = 0;
		}

		end = exp_treadle_end(raw, lo, hi);
		if (end && end != at_end) {
			at_end = end;
			quiet_ends++;
		}

		//
		// Closed the way a hand closes it: by putting the switch
		// back.  Exactly at the count rather than at or past it,
		// so this happens once however long the foot keeps going
		// - and the commit still comes from the transition above,
		// once core 1 has read the pot back into expression.
		//
		if (quiet_ends == TREADLE_SETTLED) {
			set_effect_pot(&expjack_effect, EXPJACK_CALIBRATE, 0);
			say_closed = true;
		}
	}

	if (want_lo == shown_lo && want_hi == shown_hi && !say_closed)
		return;

	//
	// The switch goes in the same message when the pedal was the one
	// that threw it, because a control that moves on its own is the
	// one thing the app cannot work out for itself.  Not otherwise:
	// a switch the host just set does not want telling about it.
	//
	uint8_t pairs[3 * 2];
	int n = 0;

	if (say_closed) {
		pairs[2 * n] = EXPJACK_CALIBRATE + 1;
		pairs[2 * n + 1] = 0;
		n++;
	}
	pairs[2 * n] = EXPJACK_HEEL + 1;
	pairs[2 * n + 1] = want_lo;
	n++;
	pairs[2 * n] = EXPJACK_TOE + 1;
	pairs[2 * n + 1] = want_hi;
	n++;

	if (sysex_echo_pots(EXPJACK_EFFECT_ID, pairs, n)) {
		shown_lo = want_lo;
		shown_hi = want_hi;
		say_closed = false;
	}
}

static void init_exp_pins(void)
{
	//
	// Take both back first.  The setting may have moved away from
	// whatever had them, and a debounce state machine left running on
	// a pin that is now an LED reads it as a switch held down - along
	// with any press it was part-way through when it stopped.
	//
	for (int sw = NR_ONBOARD_SWITCHES; sw < NR_SWITCHES; sw++) {
		pio_sm_set_enabled(pio1, sw, false);
		switch_clear(sw);
		switch_clear(LONGPRESS(sw));
	}
	exp_idle();

	switch (expression.accessory) {
	case EXP_ACC_SWITCHES:
		init_exp_switch(EXP_TIP_SWITCH);
		init_exp_switch(EXP_RING_SWITCH);
		break;
	case EXP_ACC_STOMP_LED:
		init_exp_switch(EXP_TIP_SWITCH);
		// ...with the lamp put back to whatever it was showing,
		// since a sweep calls this on its way out.
		exp_led_restore();
		break;
	case EXP_ACC_TREADLE:
		// Parked driven, and left that way to be read from.
		exp_drive_begin(exp_supply_gpio(), exp_wiper_gpio());
		exp_drive_end(exp_supply_gpio(), exp_wiper_gpio());
		break;
	}
}

//
// Which of the jack's controls this accessory actually has.
//
// Offering one that cannot work is worse than not offering it: with an
// LED on the ring there is no switch there to bind, and a row that can
// never do anything is indistinguishable from one that is broken.
//
static bool exp_control_offered(unsigned int id)
{
	switch (expression.accessory) {
	case EXP_ACC_SWITCHES:
		return id != CTRL_EXP_TREADLE;
	case EXP_ACC_STOMP_LED:
		return id == CTRL_EXP_TIP_TAP || id == CTRL_EXP_TIP_HOLD;
	case EXP_ACC_TREADLE:
		return id == CTRL_EXP_TREADLE;
	default:
		return false;
	}
}

//
// The setting is written by the app, so it moves while the pedal is
// running and the pins have to follow it there and then.  Waiting for a
// reboot means picking an accessory and finding nothing works, which is
// indistinguishable from it being broken.
//
static bool exp_follow_setting(void)
{
	static int configured = -1;

	if (expression.accessory == configured)
		return false;

	//
	// Not while a sweep has the pins.  Handing them over mid-sweep
	// puts a pull-up, a state machine or the LED's PWM on them
	// underneath it, and every reading after that is of the pedal
	// rather than of the accessory - a driven ring reads as the rail,
	// which is a stomp box with an LED classified as two switches.
	//
	// Nothing is lost by waiting: the sweep hands the pins over
	// itself when it finishes, and this runs on the pass after.
	//
	if (exp_sweep_busy())
		return false;

	configured = expression.accessory;
	init_exp_pins();
	return true;
}

//
// EXPRESSION JACK, AUTOMATICALLY.  Everything below is the Auto setting
// of the Detect pot, and does nothing at all in Manual.
//
// The whole of the policy is that a probe is only ever run when it costs
// nothing, and that its answer is only ever taken when it is better than
// what is already there.  exp_verdict() is the second half of that and
// is shared with the host's own probe; this is the first half.
//

//
// The pedal writing what is on the jack, having found out.
//
static void exp_set_accessory(int accessory)
{
	if (accessory != expression.accessory)
		set_effect_pot(&expjack_effect, EXPJACK_ACCESSORY, accessory);
}

//
// ...and telling the host, which is a separate job because it can fail.
//
// The transmit queue drops a message while it is busy and telemetry keeps
// it busy several times a second, so one attempt is a coin toss - and
// nothing else corrects it.  An accessory change sets the identity off,
// but the app rebuilds its bindings from that without asking for the
// state again, so a dropped echo leaves its menu showing an accessory the
// pedal stopped believing in minutes ago.  Which looks exactly like
// detection being stuck, and is not.
//
// So this keeps what the host has been told and says it again until it
// lands, the same shape the calibration endpoints use.  Outside Auto too:
// a stale picture is not better for being the host's own fault.
//
static void exp_tell_accessory(void)
{
	static int told = -1;
	const uint8_t pair[2] = { EXPJACK_ACCESSORY + 1, expression.accessory };

	if (told == expression.accessory)
		return;
	if (sysex_echo_pots(EXPJACK_EFFECT_ID, pair, 1))
		told = expression.accessory;
}

//
// Is a treadle sitting at the bottom of its travel?
//
// Which is when it may be probed, and only then.  A sweep takes the pins
// and the treadle's value freezes for its length: invisible at the
// bottom, where it already is and where the driven pin settles back to
// the same place, and a hole in whatever it drives anywhere else.
//
// A treadle whose heel stop reads above EXP_LOW is a treadle whose
// unplugging is never noticed.  That is a miss rather than a wrong
// answer, and the same threshold decides "this pin is at the bottom of
// its range" everywhere else here.
//
static bool exp_treadle_parked(void)
{
	return expression.accessory == EXP_ACC_TREADLE &&
	       exp_treadle_raw < EXP_LOW;
}

// How often to look at a treadle that is parked. Not continuously: a
// sweep every 24ms would mean a treadle that never reads anything else.
#define EXP_RECHECK_MS	1000

//
// Whether a sweep may run at all without costing something.
//
// An empty jack has nothing to interrupt.  A treadle only while it is
// parked, since a sweep freezes its value for its own length - invisible
// at the bottom, a hole in whatever it drives anywhere else.  A switch
// accessory only while nothing is held: restarting a state machine in
// the middle of a press loses the quiet-until-release debounce.pio keeps
// in quietzero, and the foot coming off would report a second time.
//
// The tip carries a switch on both switch accessories; the ring does on
// a dual stomp and is the lamp on the other, and a lamp has no press to
// lose.
//
static bool exp_probe_free(void)
{
	switch (expression.accessory) {
	case EXP_ACC_NONE:
		return true;
	case EXP_ACC_TREADLE:
		return exp_treadle_parked();
	case EXP_ACC_SWITCHES:
		return gpio_get(EXP_TIP_GPIO) && gpio_get(EXP_RING_GPIO);
	case EXP_ACC_STOMP_LED:
		return gpio_get(EXP_TIP_GPIO);
	}
	return false;
}

//
// When detection is finished, which is not the same as when it has an
// answer.
//
// Agreeing on a verdict was the wrong test.  A verdict is a bucket, and
// a plug on its way into the jack sits in one for a long time while it
// is still moving: the contacts leave the normalling contacts before
// they meet the plug's conductors, so "tip open, ring open" happens
// during an insertion and is also exactly a switch box at rest.  Eleven
// consecutive sweeps agreeing it is a switch box said nothing at all
// about whether the plug had arrived, and sampling more often or further
// apart is the same wrong question asked again.
//
// What separates being inserted from inserted is that the readings stop
// moving.  So that is the test, and it is about the world rather than
// about our own name for it.
//
// EXP_STEADY is against about a count of jitter on this converter, and
// the nearest two states this has to tell apart are 69 and 812.  There
// is a lot of room in between.
//
#define EXP_STEADY	64
#define EXP_SETTLE_MS	1000

static bool exp_readings_alike(const uint16_t *a, const uint16_t *b)
{
	//
	// The four that mean something.  The float pair is residual
	// charge rather than a measurement - see the comment on the
	// reading enum - and the temperature drifts and says nothing
	// about the jack.
	//
	static const uint8_t worth[] = {
		EXP_PULLUP_RING, EXP_PULLUP_TIP,
		EXP_DRIVETIP_RING, EXP_DRIVERING_TIP,
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(worth); i++) {
		int moved = a[worth[i]] - b[worth[i]];

		if (moved > EXP_STEADY || moved < -EXP_STEADY)
			return false;
	}
	return true;
}

static void exp_detect_task(void)
{
	static uint16_t last[EXP_NR_READINGS];
	static absolute_time_t steady_since, next_probe;
	static bool settled;
	absolute_time_t now = get_absolute_time();
	unsigned int every = 0;

	if (expression.detect != EXP_DETECT_AUTO)
		return;

	//
	// Not in the middle of learning a treadle's travel.  The window
	// is somebody standing on the pedal with a job half done, which
	// is no time to start reconsidering what the pedal is.
	//
	// A reading already in hand is dropped rather than saved for
	// afterwards: it would be stale by then, and a result nobody
	// takes blocks every sweep after it - exp_sweep_start() does
	// nothing until the last answer has been read.
	//
	if (expression.learning) {
		if (exp_sweep_ready())
			exp_sweep_take();
		return;
	}

	//
	// Whatever a sweep found.  sysex_send_exp() runs before this and
	// takes the result it asked for, so whatever is still here is
	// this task's - and if it took ours, the next pass starts another.
	//
	// exp_verdict() is the state machine: it confirms a reading
	// consistent with what is set, names the jack when it is not, and
	// answers UNKNOWN rather than guessing.  So finding, losing and
	// changing an accessory are all the same line - once the jack has
	// stopped moving under it.
	//
	if (exp_sweep_ready()) {
		const uint16_t *now_read = exp_sweep_take();

		if (!exp_readings_alike(now_read, last)) {
			memcpy(last, now_read, sizeof(last));
			steady_since = now;
			settled = false;
		} else if (!settled &&
			   absolute_time_diff_us(steady_since, now) >
			   EXP_SETTLE_MS * 1000) {
			int found = exp_verdict(now_read);

			if (found != EXP_ACC_UNKNOWN)
				exp_set_accessory(found);
			settled = true;
		}
	}

	//
	// Nothing below here may look at a pin the sweep is holding: what
	// gpio_get() reads then is the sweep's own drain or pull-up, and
	// exp_treadle_raw stopped being updated when the sweep started.
	//
	if (exp_sweep_busy())
		return;

	//
	// And when to look.
	//
	// Until it has settled, as fast as the sweep will go, whatever is
	// set - including at startup, where what is set came out of the
	// globals and nobody has looked at it.  After that an empty jack
	// keeps watching for something to arrive, a parked treadle is
	// looked at now and again so that unplugging it is noticed, and a
	// switch accessory is left alone: that one announces its own
	// departure through the long press the normalling contacts make.
	//
	if (settled) {
		switch (expression.accessory) {
		case EXP_ACC_NONE:
			break;
		case EXP_ACC_TREADLE:
			every = EXP_RECHECK_MS;
			break;
		default:
			return;
		}
	}

	if (!exp_probe_free())
		return;
	if (every && absolute_time_diff_us(now, next_probe) > 0)
		return;
	next_probe = delayed_by_ms(now, every);

	exp_sweep_start();
}

static void exp_init(void)
{
	adc_init();
	exp_idle();
}

#endif	// EXP_TIP_GPIO
#endif
