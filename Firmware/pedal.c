#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

#include "board.h"

#include "status.h"
#include "debounce.pio.h"
#include "rotary.pio.h"
#include "i2s.pio.h"

#define PIO0_I2S_TX_SM 0
#define PIO0_I2S_RX_SM 1
#define PIO0_WS2812_SM 2

// PIO1 runs one debounce state machine per switch, and the state
// machine index is the switch id - see switch.h.  PIO2 has the one
// rotary encoder.
#define ROTARY_SM 0

#define PWM_WRAP 4096	// Entirely arbitrary

#include "Audio/types.h"
#include "Audio/util.h"
#include "Audio/envelope.h"
#include "Audio/single-pole.h"
#include "Audio/biquad.h"
#include "Audio/fft.h"
#include "Audio/analyze.h"
#include "tac5112.h"

//
// After the state machine numbering above, which it uses.
//
// Only on a board that has them.  pixels.h is the whole WS2812B driver
// and is written against NR_LEDS and WS2812_GPIO, so on a board with a
// plain PWM LED there is nothing here to compile - every caller is
// already behind the same #ifdef.
//
#ifdef WS2812_GPIO
#include "pixels.h"
#endif


#include "midi/midi.h"
#include "midi/uart.h"
#include "tusb.h"
#include "usb-audio.h"
#include "switch.h"

static int tuner_mode = 0;
static volatile int user_interaction = 0;

#include "Audio/effect.h"

uint8_t effect_chain[MAX_ROUTED_EFFECTS];
uint8_t routed_effect_count = 0;

#include "effect-state.h"

#include "scene.h"
#include "hardware.h"
#include "exp.h"
#include "midi/sysex.h"

#include "ui.h"

static inline void enable_ftz(void)
{
	// FZ bit (24) in FPSCR flushes subnormal results to zero in hardware,
	// covering every float op in the audio chain. Without it, any feedback
	// path that decays into sub-1e-38 range causes a 5-20x FPU slowdown on
	// Cortex-M33 (VFPv5 handles subnormals in hardware, not via trap, but
	// still at a significant penalty). Must be set per-core.
	uint32_t fpscr;

	fpscr = __builtin_arm_get_fpscr();
	fpscr |= 1u << 24;
	__builtin_arm_set_fpscr(fpscr);
}

static void __audio_func(audio_processing)(void)
{
	enable_ftz();
	init_meters();
	for (;;)
		make_one_noise();
}

unsigned get_audio_samples(int32_t *buffer, unsigned nr)
{
	return get_output_samples((s32 *)buffer, nr);
}

static void init_effects(void)
{
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		reset_effect(effect);
	}

	//
	// The settings first, because they are the pedal's and not the
	// scene's - which of them is the MIDI channel should not depend
	// on which scene happens to load next.
	//
	load_globals();

	//
	// No fallback for a store with nothing in it, because there is
	// nothing to fall back to.  A slot that fails its hash, or was
	// never written, simply does not load and every effect keeps the
	// defaults reset_effect() just gave it - which leaves a new pedal
	// with everything at its default and nothing routed.  That is the
	// right answer, and guessing a chain would be worse.
	//
	load_scene(0);

	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *effect = effects[i];
		effect->init(effect->pot_values[0]);
	}
}

#include "tuner.h"

//
// How long boot is allowed to take before it is called a hang.
//
// Everything between here and the main loop is i2c probes with 2ms
// timeouts, some table building and a flash read, so the real figure is
// milliseconds and this is all margin.  It has to stay well clear of
// the truth in the other direction too: a boot that legitimately took
// longer than this would reach BOOTSEL instead of playing, which is a
// worse failure than the one being guarded against.
//
#define BOOT_WATCHDOG_MS 3000

int main()
{
	//
	// A pedal that hangs before its main loop is a brick.
	//
	// It has no USB, so it is not a device and not in BOOTSEL and the
	// host logs nothing at all - from the other end nothing was ever
	// plugged in.  It has no audio either, so the only way back is the
	// BOOTSEL button, and on a board in an enclosure that is a screw-
	// driver.  This has been seen occasionally for a long time without
	// ever being pinned down, which is partly because every occurrence
	// destroys the evidence and costs a disassembly.
	//
	// So the hang is made survivable rather than diagnosed: arm a
	// watchdog before anything that could hang, and if the previous
	// boot never got far enough to disarm it, ask the bootrom for
	// BOOTSEL instead of trying again.  A hung pedal then comes back
	// as something picotool can talk to, and the next attempt costs a
	// reflash rather than a screwdriver.
	//
	// watchdog_enable_caused_reboot() is specifically a *timeout*, not
	// any reset - a deliberate watchdog_reboot() sets a different
	// magic - so this cannot be tripped by anything asking for a
	// restart on purpose.
	//
	//
	// The one thing that runs earlier than the watchdog can.
	//
	// The guard below is armed *here*, so it cannot catch anything that
	// happens before it - the bootrom, crt0, the clock and PLL setup,
	// XIP coming up.  A hang there is invisible to it, and that is not
	// hypothetical: the sixth occurrence came back as no USB device and
	// nothing in BOOTSEL, which is what a watchdog that never got armed
	// looks like from outside.
	//
	// So light the LED first, with a plain GPIO write.  PWM wants a
	// clock configured and this deliberately wants nothing at all -
	// it is here to run before everything, including the thing that
	// exists to catch failures.  init_pwm_pins() takes the pin over
	// later and keeps it lit; the first set_led() in the main loop is
	// what finally turns it into a status light.
	//
	// Which makes the lamp answer a question nothing else can:
	//
	//	dark and stays dark	never reached main() at all
	//	lit and stays lit	reached main(), hung during boot
	//	lit, then dims		booted; that is the UI taking over
	//
	//
	// ...on a board that has a plain LED on that pin.  The usb-stomp
	// board does not: LED_GPIO there is the stomp switch, wired
	// straight to ground with no series resistor, so driving it high
	// and then standing on the switch is a short.  Those boards have
	// three WS2812Bs instead, which cannot be lit this early - they
	// want PIO, DMA and a configured clock, all of which is the thing
	// this lamp exists to run before.
	//
#ifndef WS2812_GPIO
	gpio_init(LED_GPIO);
	gpio_set_dir(LED_GPIO, GPIO_OUT);
	gpio_put(LED_GPIO, 1);
#endif

	if (watchdog_enable_caused_reboot())
		reset_usb_boot(0, 0);

	watchdog_enable(BOOT_WATCHDOG_MS, false);

	enable_ftz();

	init_i2s();
	init_ws2812();
	init_sw_pins();
	init_pwm_pins();
	init_rotary_encoder();
#ifdef EXP_TIP_GPIO
	exp_init();
#endif
	init_i2c_bus(i2c0, 400, I2C0_SDA, I2C0_SCL);
	init_i2c_bus(i2c1, 400, I2C1_SDA, I2C1_SCL);

	//
	// Before init_usb(), because it decides what the pedal enumerates
	// as.  USB is a hotplug bus and the host may already be attached
	// and waiting, so the name wants to exist before the device does.
	// The i2c buses above are all this needs.
	//
	probe_hardware();

	init_usb();
	uart_midi_init();

	absolute_time_t now = get_absolute_time();

	absolute_time_t next_ui_update = delayed_by_ms(now, 50);

	//
	// Early boards need their codec set up; the current ones strap it
	// in hardware.  Unconditional because the first thing it does is
	// ask whether there is a TAC5112 there to talk to, which is the
	// same question as whether this is one of those boards.
	//
	tac5112_init();

	init_effects();
#ifdef EXP_TIP_GPIO
	init_exp_switches();	// wants settings.exp_jack, loaded just above
#endif

	multicore_launch_core1(audio_processing);

	//
	// Booted.  Everything from here is the main loop, which has its own
	// ways of going wrong and is not what this was guarding.
	//
	watchdog_disable();

	for (;;) {
		absolute_time_t now = get_absolute_time();

		//
		// Everything the outside world asks for is taken in
		// here, and acted on here, so a sender further down can
		// never have the state it is reporting changed under it.
		//
		tud_task();
		usb_midi_poll();
		uart_midi_poll();

		sysex_send_identity();
		sysex_send_telemetry();
#ifdef EXP_TIP_GPIO
		sysex_send_exp();
#endif
		sysex_send_schema();
		sysex_send_state_dump();
		sysex_send_status();

		//
		// Hand the queue whatever USB will take right now.
		//
		// Between the senders above and usb_audio_task() below on
		// purpose.  A sender builds a whole reply into the queue in
		// one pass, which is what keeps it from reporting a mixture
		// of before and after; this hands over a few packets of it
		// and returns the moment the endpoint is full.  So a reply
		// the size of the schema costs many short passes through
		// here instead of one long one, and the audio endpoint gets
		// fed on time in between them.
		//
		midi_tx_drain();
		usb_audio_task();

		// Claim 25Hz screen updates
		if (now > next_ui_update) {
			next_ui_update = delayed_by_ms(now, 40);

			//
			// Whatever the switches are bound to.
			//
			// Ahead of the tuner check on purpose: update_ui()
			// does not run in tuner mode, and a gesture bound to
			// ACT_TUNER has to be able to turn it off again.  A
			// side effect is that a switch now acts while the
			// tuner is up rather than being queued until it is
			// dismissed, which is the more predictable of the
			// two behaviours anyway.
			//
			handle_switch_bindings();

			//
			// Are we in tuner mode?
			//
			// The transition out is caught here rather than at
			// the three places that clear tuner_mode, because
			// this is the one spot that sees every route out of
			// it - a footswitch, a CC, or anything added later -
			// and what is owed on the way out is the same
			// whichever it was.
			//
			static bool was_tuning = false;

			if (tuner_mode) {
				was_tuning = true;
				tuner_mode_ui();
				continue;
			}
			if (was_tuning) {
				was_tuning = false;
				tuner_silence();
			}

			update_ui(to_ms_since_boot(now));
		}
	}
}
