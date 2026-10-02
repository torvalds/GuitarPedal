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

//
// What gets reported is what was *observed*.  Any inference from it -
// which board this is, how old - belongs to whoever is reading rather
// than in the wire format, so that being wrong about it later costs an
// app change and not a protocol one.
//
static struct {
	bool i2c_codec;		// a codec answered, so it is not strapped
	bool legacy_screen;	// SH1106, 0x3c - a design that is gone
#ifdef NRF54_SWDIO
	uint32_t radio;		// the nRF54's debug port id, or zero
#endif
} hardware;

//
// One i2s state machine's DMA.
//
// All four are the same channel with the direction flipped.  Memory is
// the side that increments and the side the ring wraps, and which of
// read or write that is depends on which way the samples are going.
//
static int i2s_dma_channel(uint sm, bool is_tx, raw_sample_t *buf)
{
	int chan = dma_claim_unused_channel(true);
	dma_channel_config c = dma_channel_get_default_config(chan);

	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_read_increment(&c, is_tx);
	channel_config_set_write_increment(&c, !is_tx);
	channel_config_set_dreq(&c, pio_get_dreq(pio0, sm, is_tx));
	channel_config_set_ring(&c, !is_tx, 7);	// 128 bytes, the buffer

	pio_sm_clear_fifos(pio0, sm);

	if (is_tx)
		dma_channel_configure(chan, &c, &pio0->txf[sm], buf,
				      0xffffffff, false);
	else
		dma_channel_configure(chan, &c, buf, &pio0->rxf[sm],
				      0xffffffff, false);

	return chan;
}

#ifdef NRF54_SWDIO
//
// The radio's audio link, which nothing reads or writes yet: the nRF's
// i2s is described in its devicetree and driven by nothing, so this
// carries silence and exists to prove the pins.
//
static raw_sample_t __attribute__((aligned(128))) nrf54_i2s_buf[16];
static int nrf54_dma_tx, nrf54_dma_rx;
#endif

//
// Every i2s link on the board, started together.
//
// This runs after probe_hardware() because the radio's half depends on
// what that found.  GPIO10 is the radio's data pin on this revision and
// was the footswitch on the one before it, and the two share a board
// file, so driving it on the older board would put a PIO output against
// a switch.
//
// Starting them together is what the one PIO block buys beyond the
// instruction memory: four state machines off one clock, enabled on the
// same cycle, so the two links share a starting edge rather than only a
// rate.
//
static void init_i2s(void)
{
	uint tx_offset = pio_add_program(pio0, &i2s_tx_program);
	uint rx_offset = pio_add_program(pio0, &i2s_rx_program);
	uint32_t sms, dmas;

	//
	// BCLK and FSYNC are the side-set, so they and only they have to be
	// adjacent.  Which of the two is lower is a board fact that reaches
	// the instructions rather than this call; see i2s.pio.
	//
	_Static_assert(I2S_BCLK == I2S_FSYNC + 1 || I2S_FSYNC == I2S_BCLK + 1,
		       "i2s BCLK and FSYNC must be adjacent for the side-set");
#ifdef I2S_FSYNC_BELOW_BCLK
	_Static_assert(I2S_FSYNC < I2S_BCLK,
		       "I2S_FSYNC_BELOW_BCLK disagrees with the pins");
#else
	_Static_assert(I2S_BCLK < I2S_FSYNC,
		       "the side-set wants I2S_FSYNC_BELOW_BCLK for these pins");
#endif

	i2s_tx_program_init(pio0, PIO0_I2S_TX_SM, tx_offset,
			    I2S_BCLK, I2S_FSYNC, I2S_DIN, I2S_DOUT);
	i2s_rx_program_init(pio0, PIO0_I2S_RX_SM, rx_offset,
			    I2S_FSYNC, I2S_DOUT);

	dma_rx = i2s_dma_channel(PIO0_I2S_RX_SM, false, i2s_dma_buf);
	dma_tx = i2s_dma_channel(PIO0_I2S_TX_SM, true, i2s_dma_buf);

	sms = (1u << PIO0_I2S_TX_SM) | (1u << PIO0_I2S_RX_SM);
	dmas = (1u << dma_rx) | (1u << dma_tx);

#ifdef NRF54_SWDIO
	if (hardware.radio) {
		_Static_assert(NRF54_I2S_BCLK == NRF54_I2S_FSYNC + 1 ||
			       NRF54_I2S_FSYNC == NRF54_I2S_BCLK + 1,
			       "the radio's BCLK and FSYNC must be adjacent too");
#ifdef I2S_FSYNC_BELOW_BCLK
		_Static_assert(NRF54_I2S_FSYNC < NRF54_I2S_BCLK,
			       "the radio's pins disagree with I2S_FSYNC_BELOW_BCLK");
#else
		_Static_assert(NRF54_I2S_BCLK < NRF54_I2S_FSYNC,
			       "the radio's pins disagree with I2S_FSYNC_BELOW_BCLK");
#endif
		i2s_tx_program_init(pio0, PIO0_NRF54_I2S_TX_SM, tx_offset,
				    NRF54_I2S_BCLK, NRF54_I2S_FSYNC,
				    NRF54_I2S_DIN, NRF54_I2S_DOUT);
		i2s_rx_program_init(pio0, PIO0_NRF54_I2S_RX_SM, rx_offset,
				    NRF54_I2S_FSYNC, NRF54_I2S_DOUT);

		nrf54_dma_rx = i2s_dma_channel(PIO0_NRF54_I2S_RX_SM, false,
					       nrf54_i2s_buf);
		nrf54_dma_tx = i2s_dma_channel(PIO0_NRF54_I2S_TX_SM, true,
					       nrf54_i2s_buf);

		sms |= (1u << PIO0_NRF54_I2S_TX_SM) |
		       (1u << PIO0_NRF54_I2S_RX_SM);
		dmas |= (1u << nrf54_dma_rx) | (1u << nrf54_dma_tx);
	}
#endif

	//
	// The DMA first, so the transmit fifos have something in them
	// before the state machines start reading them.
	//
	// RX and TX start at the same point, together.  But TX will fill up
	// the PIO buffers and move ahead, while RX will be waiting for the
	// first samples to come in, so it naturally falls behind - and in a
	// circular buffer that is the same as being ahead.
	//
	dma_start_channel_mask(dmas);
	pio_enable_sm_mask_in_sync(pio0, sms);
}

static void init_ws2812(void)
{
#ifdef WS2812_GPIO
	pixels_init();
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
// What this firmware found itself running on.
//
// Probed once at boot, and the question it answers is not "which board
// is this" - the pin map already settled that at compile time, and a
// fixed build cannot adapt to landing on the wrong one anyway.  It is
// the narrower question of what is on the far end of the FFC, which the
// build genuinely does not know and must not guess.
//
// The audio-jacks board is a separate board joined by a cable, and it
// comes in two flavours: a TAC5112, which was never wired for stereo,
// and a TAC5242, which was.  Either can be paired with either MCU board,
// so which one is present is not a property of the build and cannot be.
// The TAC5112 needs its control registers set up over i2c0 regardless,
// so the firmware has to find out - and having found out, it can say so.
//
// **Mono against stereo is the difference a person actually notices**,
// and it is this one.  It is not the codec's doing - both parts are
// stereo-capable - it is that the older board only ever routed one
// channel.
//
// The SH1106 screen on i2c1 is the same kind of statement: it belonged
// to a design that is gone, the code for it went with it, and the part
// still answers when addressed.
//
// The eeprom used to be probed here too, and is not any more.  It was
// the scene store, which now lives in the RP2354's own flash; after that
// it survived a while as a hint about which board this was, and it was
// never a good one.  It sat on whichever board happened to carry it
// across a couple of revisions, so its presence identified nothing, and
// the reading was not even stable - see the issue list.

//
// Whether an effect has the board under it that it asked for.
//
// The generator wrote the list of who wants what; board.h turned each
// name into an answer.  Expanded here because this is the first place
// that has both - the effect headers are compiled by the host bench
// too, and there is no board under that.
//
// An absent effect is still in effects[] and still has its pots: taking
// it out would renumber every effect above it, and the ids are the
// wire.  What it loses is its card in the app and its pots in the
// state dump.
//
static bool effect_present(unsigned int id)
{
	switch (id) {
#define HW_NEEDS(id, what)	case id: return !!(HAVE_##what);
	EFFECT_HW_LIST
#undef HW_NEEDS
	default:
		return true;
	}
}

static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr)
{
	uint8_t byte;

	// One byte, harmless to anything that does answer, and a timeout
	// rather than a hang if the bus is being held down.
	return i2c_read_timeout_us(i2c, addr, &byte, 1, false, 2000) == 1;
}

static void probe_hardware(void)
{
	hardware.i2c_codec = i2c_probe(TAC5112_I2C);
#ifdef SH1106_I2C
	hardware.legacy_screen = i2c_probe(SH1106_I2C);
#endif

	//
	// Say what we are before USB exists, because the name is part of
	// how a person tells two pedals apart and the host may already be
	// attached and waiting.
	//
	// The board name is compile-time and what the probe means is not,
	// so this is where the two meet.
	//
	// **What that bit means is a board fact, so the board says it.**
	// The probe establishes one thing - a codec answered on i2c rather
	// than being strapped - and the consequence differs: on the split
	// family the i2c part is the TAC5112, which is mono; on minimal it
	// is the TAC5212, which is DC-coupled where the strapped one has a
	// corner at 10Hz.  Both are worth knowing and neither is the other.
	//
	// A board header that defines CODEC_I2C_PRODUCT gives its i2c-codec
	// build that name, followed by the serial's last four digits.
	//
#ifdef CODEC_I2C_PRODUCT
	if (hardware.i2c_codec) {
		static char product[sizeof(CODEC_I2C_PRODUCT) + 5];
		char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
		size_t len;

		pico_get_unique_board_id_string(id, sizeof(id));
		len = strlen(id);
		memcpy(product, CODEC_I2C_PRODUCT " ", sizeof(CODEC_I2C_PRODUCT));
		memcpy(product + sizeof(CODEC_I2C_PRODUCT), id + len - 4, 5);
		usb_set_product(product);
	} else
#endif
	usb_set_product(hardware.i2c_codec
			? PEDAL_BOARD_NAME " " CODEC_I2C_DESC " Pedal"
			: PEDAL_BOARD_NAME " " CODEC_STRAPPED_DESC " Pedal");

	//
	// Stated rather than judged, for the same reason the wire format
	// carries the observation: "early" is an inference and it is the
	// reader's to draw.
	//
	report_status(hardware.i2c_codec
		      ? "Codec on i2c: " CODEC_I2C_DESC
		      : "Codec strapped: " CODEC_STRAPPED_DESC);
	if (hardware.legacy_screen)
		report_status("A screen answered on i2c");

#ifdef NRF54_SWDIO
	//
	// After the i2c probes because it takes a millisecond of reset,
	// and this is the last thing before USB.
	//
	// It says the chip is there, not that it is running anything -
	// the debug port answers on a part that has never been
	// programmed, and that is most of what makes it worth asking.
	//
	hardware.radio = nrf54_probe();
	report_status(hardware.radio ? "The radio answered on SWD"
				    : "No radio on SWD");

	//
	// The MIDI port to the radio, which only exists when a radio
	// answered.  Its i2s waits for the same answer and is started by
	// init_i2s(), with the codec's.
	//
	if (hardware.radio)
		nrf54_uart_init();
#endif
}
static uint debounce_offset;

static void init_sw_pins(void)
{
	PIO pio = pio1;

	debounce_offset = pio_add_program(pio, &debounce_program);


	//
	// Same PIO program for every switch, one state machine each,
	// walked in switch id order so that state machine N really is
	// switch N.  switch_irq() relies on that and has no other way
	// to know which pin a fifo entry came from.
	//
	// Only the ones soldered to this board.  Anything on the
	// expression jack is an accessory rather than bring-up, and waits
	// for init_exp_switches() and the setting that decides it.
	//
	for (int sw = 0; sw < NR_ONBOARD_SWITCHES; sw++) {
		init_sw_pin(pio, switch_gpio[sw]);
		debounce_program_init(pio, sw, debounce_offset, switch_gpio[sw]);
	}


	irq_set_exclusive_handler(PIO1_IRQ_0, switch_irq);
	irq_set_enabled(PIO1_IRQ_0, true);
}

// Any pin, not just an LED_GPIO: a board with smart LEDs still has an
// expression jack that may have something to light on it.  A board with
// neither has nothing to dim at all.
#if !defined(WS2812_GPIO) || defined(EXP_TIP_GPIO)
static void init_one_pwm_pin(int pin)
{
	unsigned int slice = pwm_gpio_to_slice_num(pin);

	gpio_set_function(pin, GPIO_FUNC_PWM);
	pwm_set_wrap(slice, PWM_WRAP);
	pwm_set_gpio_level(pin, 0);
	pwm_set_enabled(slice, true);
}
#endif

static void init_pwm_pins(void)
{
#ifdef WS2812_GPIO
	//
	// Nothing to dim.  A board with smart LEDs has no PWM one - and on
	// the usb-stomp board LED_GPIO is not an LED at all, it is the
	// stomp switch, shorting to ground with no series resistor.  So
	// this must not run there, and neither must the boot lamp in
	// main().  Both are left in place for the boards that do have it.
	//
	return;
#else
	init_one_pwm_pin(LED_GPIO);

	//
	// Full, not off, and it stays that way until the first UI tick.
	//
	// main() lights this pin as a plain GPIO before anything else runs,
	// and taking it over for PWM would drop it - so the level is put
	// back up here and the LED stays on across the handover.  What that
	// buys is a lamp that means "started, not finished yet": it comes on
	// at the first instruction and goes to its real brightness when
	// set_led() first runs, which is inside the main loop.
	//
	// So a pedal that hangs during boot sits there lit, and a pedal that
	// never got as far as main() sits there dark.  See the boot comment
	// in main() for why that distinction is the one worth having.
	//
	pwm_set_gpio_level(LED_GPIO, PWM_WRAP);
#endif
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

#ifdef ROTARY_A_GPIO
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


#endif
