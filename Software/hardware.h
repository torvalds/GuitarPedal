#ifndef HARDWARE_H
#define HARDWARE_H

//
// Bringing the board up, and finding out which board it is.
//
// include/board.h says *which pins*.  This says *how to start them*: the
// i2s state machines and their DMA, the WS2812 program, one debounce
// state machine per switch, the PWM the LEDs are dimmed with, and the
// rotary encoder's quadrature decoder.
//
// It also probes what is on the i2c bus, which is a different question
// and lives here because it is the same one: what is actually out there.
// A fixed build cannot adapt to the board it lands on and does not try -
// the answer goes out in the identity reply so that "is this the board
// this firmware was built for" can be asked of a running pedal.
//
// Ordering note, since this is one translation unit and include order is
// program order: this has to come before midi/sysex.h, because the
// identity reply reports what probe_hardware() found.
//

static void init_i2s(void)
{
	uint tx_offset, rx_offset;

	tx_offset = pio_add_program(pio0, &i2s_tx_program);
	rx_offset = pio_add_program(pio0, &i2s_rx_program);

	i2s_tx_program_init(pio0, PIO0_I2S_TX_SM, tx_offset, I2S_BCLK);
	i2s_rx_program_init(pio0, PIO0_I2S_RX_SM, rx_offset, I2S_BCLK);

	dma_rx = dma_claim_unused_channel(true);
	dma_channel_config c_rx = dma_channel_get_default_config(dma_rx);
	channel_config_set_transfer_data_size(&c_rx, DMA_SIZE_32);
	channel_config_set_read_increment(&c_rx, false);
	channel_config_set_write_increment(&c_rx, true);
	channel_config_set_dreq(&c_rx, pio_get_dreq(pio0, PIO0_I2S_RX_SM, false));
	channel_config_set_ring(&c_rx, true, 7); // write wrap at 128 bytes (32 words)

	dma_tx = dma_claim_unused_channel(true);
	dma_channel_config c_tx = dma_channel_get_default_config(dma_tx);
	channel_config_set_transfer_data_size(&c_tx, DMA_SIZE_32);
	channel_config_set_read_increment(&c_tx, true);
	channel_config_set_write_increment(&c_tx, false);
	channel_config_set_dreq(&c_tx, pio_get_dreq(pio0, PIO0_I2S_TX_SM, true));
	channel_config_set_ring(&c_tx, false, 7); // read wrap at 128 bytes (32 words)

	pio_sm_clear_fifos(pio0, PIO0_I2S_RX_SM);
	pio_sm_clear_fifos(pio0, PIO0_I2S_TX_SM);

	// RX and TX start at the same point, together. But TX will
	// fill up the PIO buffers and move ahead, while RX will be
	// waiting for the first samples to come in, so it naturally
	// falls behind.
	//
	// And "falls behind" is the same as "is ahead" in a circular
	// buffer.
	dma_channel_configure(dma_rx, &c_rx, i2s_dma_buf, &pio0->rxf[PIO0_I2S_RX_SM], 0xffffffff, false);
	dma_channel_configure(dma_tx, &c_tx, &pio0->txf[PIO0_I2S_TX_SM], i2s_dma_buf, 0xffffffff, false);

	dma_start_channel_mask((1u << dma_rx) | (1u << dma_tx));
}

static void init_ws2812(void)
{
#ifdef WS2812_GPIO
	uint offset = pio_add_program(pio0, &ws2812_program);
	ws2812_program_init(pio0, PIO0_WS2812_SM, offset, WS2812_GPIO);
#endif
}

// Initialize a pin for input, pulled up
static void init_sw_pin(PIO pio, int pin)
{
	gpio_init(pin);
	gpio_set_dir(pin, false);
	gpio_pull_up(pin);
	pio_gpio_init(pio, pin);
}

// I have no good way to detect USB when in USB host mode.
//
// In a perfect world, I would have a GPIO that would tell
// me whether the power is provided by the 9V guitar power
// supply or the USB line, but ...
static inline bool usb_is_connected(void)
{
	return tud_ready();
}

// We use PIO1 for the switches.
//
// They share the same program, just a separate state machine
// for each pin - state machine N is switch id N, see switch.h.
static void switch_irq(void)
{
	PIO pio = pio1;

	for (int sw = 0; sw < NR_SWITCHES; sw++) {
		if (pio_sm_is_rx_fifo_empty(pio, sw))
			continue;

		int bit = pio_sm_get(pio, sw) ? LONGPRESS(sw) : sw;
		switch_val |= 1u << bit;
	}

	user_interaction = 1;
}
//
// What this firmware is, and what it found itself running on.
//
// The hardware half is probed once at boot.  A fixed build cannot adapt
// to the board it lands on and this does not try to - it answers a
// different question, which has cost an evening more than once: is this
// the board this firmware was built for at all?
//
// The early boards carried a TAC5112 codec with its control registers on
// i2c0, and an SH1106 screen on i2c1.  Neither is supported any more and
// the code for both is gone, but the parts still answer when addressed.
// So anything replying there means the firmware is newer than the board,
// and nothing else in the system is in a position to notice.
//
// The eeprom is in the same position now.  It was the scene store, and
// which part was fitted mattered a great deal - the sizes differ in how
// many address bytes they take, so a mismatched build wrote to the wrong
// place and the pedal ran perfectly and forgot everything on reboot.
// Scenes live in the RP2354's own flash now and nothing reads the part
// at all, so all that is left is the same statement as the other two:
// something is answering at 0x50.
//
// What gets reported is what was *observed*.  Any inference from it -
// which board this is, how old - belongs to whoever is reading rather
// than in the wire format, so that being wrong about it later costs an
// app change and not a protocol one.
//
static struct {
	bool eeprom;		// the old scene store, 0x50
	bool legacy_codec;	// TAC5112, 0x51 - an early board
	bool legacy_screen;	// SH1106, 0x3c - ditto
} hardware;

static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr)
{
	uint8_t byte;

	// One byte, harmless to anything that does answer, and a timeout
	// rather than a hang if the bus is being held down.
	return i2c_read_timeout_us(i2c, addr, &byte, 1, false, 2000) == 1;
}

static void probe_hardware(void)
{
	hardware.eeprom = i2c_probe(MC24Cxx_I2C);
	hardware.legacy_codec = i2c_probe(TAC5112_I2C);
	hardware.legacy_screen = i2c_probe(SH1106_I2C);

	//
	// The one inference drawn from any of this, and it is drawn here
	// rather than on the wire.  A name is allowed to guess: nothing
	// depends on it being right beyond a human reading a port list,
	// and being wrong costs a rebuild.  The identity reply is not
	// allowed to, for the reason above - so it keeps reporting that
	// nothing answered at 0x51, and this says what that means.
	//
	// It has to be said before init_usb(), which is why probing
	// happens as early as it does.
	//
	usb_set_product(hardware.legacy_codec ? "TAC5112 Pedal" : "TAC5242 Pedal");

	//
	// Worst first, and only one of these arrives: report_status() is a
	// plain overwrite and get_status() takes the message away as it
	// reads it, so a chain of ifs would deliver the last thing tested
	// rather than the thing worth saying.
	//
	// An early board is merely old: the TAC5112 wants a little setup,
	// which it gets, and those boards never routed the second
	// channel, so they are mono.
	//
	// A missing eeprom used to be the thing worth saying, because it
	// meant nothing would persist.  It says nothing now - scenes are
	// in the RP2354's flash, which is on the die and cannot be
	// absent - so the boards built without one have stopped being
	// useless and stopped needing a warning.
	//
	if (hardware.legacy_codec || hardware.legacy_screen)
		report_status("Early board: mono only");
}
static void init_sw_pins(void)
{
	PIO pio = pio1;
	uint offset = pio_add_program(pio, &debounce_program);

	//
	// Same PIO program for every switch, one state machine each,
	// walked in switch id order so that state machine N really is
	// switch N.  switch_irq() relies on that and has no other way
	// to know which pin a fifo entry came from.
	//
	for (int sw = 0; sw < NR_SWITCHES; sw++) {
		init_sw_pin(pio, switch_gpio[sw]);
		debounce_program_init(pio, sw, offset, switch_gpio[sw]);
	}

	irq_set_exclusive_handler(PIO1_IRQ_0, switch_irq);
	irq_set_enabled(PIO1_IRQ_0, true);
}

static void init_one_pwm_pin(int pin)
{
	unsigned int slice = pwm_gpio_to_slice_num(pin);

	gpio_set_function(pin, GPIO_FUNC_PWM);
	pwm_set_wrap(slice, PWM_WRAP);
	pwm_set_gpio_level(pin, 0);
	pwm_set_enabled(slice, true);
}

static void init_pwm_pins(void)
{
	init_one_pwm_pin(LED_GPIO);
	pwm_set_gpio_level(LED_GPIO, 0);
}

static void init_i2c_bus(i2c_inst_t *i2c, int kbps, int sda, int scl)
{
	i2c_init(i2c, kbps * 1000);
	gpio_set_function(sda, GPIO_FUNC_I2C);
	gpio_set_function(scl, GPIO_FUNC_I2C);
	gpio_pull_up(sda);
	gpio_pull_up(scl);
}

//
// The one rotary encoder.  Turning it changes the selected pot's value,
// and that is all a turn has ever meant to anything but the old EQ.
//
// Accumulated by the interrupt, drained by update_ui().  There used to
// be a second encoder for picking the effect; it is gone, and picking
// the effect is done over MIDI.
//
static volatile int rotary_value;

static void rotary_irq(void)
{
	// Initial impossible previous value
	static int prev_value = 4;
	static const int lookup[32] = {
		// CW: 00 -> 10 -> 11 -> 01 -> 00
		[2] = 1, [11] = 1, [13] = 1, [4] = 1,
		// CCW: 00 -> 01 -> 11 -> 10 -> 00
		[1] = -1, [7] = -1, [14] = -1, [8] = -1
	};

	while (!pio_sm_is_rx_fifo_empty(pio2, ROTARY_SM)) {
		int curr = pio_sm_get(pio2, ROTARY_SM) & 3;
		int prev = prev_value;

		int val = lookup[(prev << 2) | curr];
		prev_value = curr;

		if (!val)
			continue;

		rotary_value += val;
	}
	user_interaction = 1;
}

// We'll use a separate PIO program for the rotary
// encoder pins eventually
static void init_rotary_encoder(void)
{
	PIO pio = pio2;
	uint offset = pio_add_program(pio, &rotary_program);

	// The program reads both pins of the quadrature pair starting
	// at the one it is given, so A and B have to stay adjacent.
	_Static_assert(ROTARY_B_GPIO == ROTARY_A_GPIO + 1,
		       "the quadrature pair has to be adjacent");

	init_sw_pin(pio, ROTARY_A_GPIO);
	init_sw_pin(pio, ROTARY_B_GPIO);
	rotary_program_init(pio, ROTARY_SM, offset, ROTARY_A_GPIO);

	irq_set_exclusive_handler(PIO2_IRQ_0, rotary_irq);
	irq_set_enabled(PIO2_IRQ_0, true);
}


#endif
