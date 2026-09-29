#ifndef NRF54_SWD_H
#define NRF54_SWD_H

//
// SWD - ARM's two-wire Serial Wire Debug - to the nRF54L10.
//
// Two callers, wanting different amounts of it.  The boot probe below
// needs a single register read to answer "is there a radio on the other
// end of these wires".  Programming the part is the host's job over
// CMSIS-DAP, and that is the same transactions with somebody else
// choosing them.  This file has the transactions; what to do with them
// is not here.
//
// Two things about this part, both read out of probe-rs's nRF54L
// target rather than observed.  Its debug port is ADIv5, the older of
// ARM's two debug architectures, with the access port at index 0; and
// it needs no dormant-state wake-up sequence before the ordinary
// JTAG-to-SWD switch, because probe-rs does not override the default
// connection sequence for it.
//
// Nothing here has run against hardware.
//

#ifdef NRF54_SWDIO

//
// Half a clock period, counted in nops.
//
// Slow on purpose to start with: a bit-banged line that works is worth
// more on a board nobody has run yet than a fast one that is marginal,
// and it can be wound down to a few kilohertz to be looked at with a
// scope.  PIO is the right way to drive this once it is known to work
// at all - it is how the speed gets to where a bulk transfer would
// notice - and swd_transfer() is the seam that change happens behind.
//
static uint32_t swd_half_period = 48;

static inline void swd_delay(void)
{
	for (uint32_t i = 0; i < swd_half_period; i++)
		__asm__ volatile ("nop");
}

//
// What the host asked for in DAP_SWJ_Clock, as near as a nop loop can
// offer it.
//
// A turn of the loop is not one cycle: it is the nop, an increment, a
// compare and a branch, so roughly four.  Dividing by that is what
// makes the answer the right order of magnitude rather than four times
// too slow - and the fixed subtraction on top is the pin writes either
// side, which happen once per bit however long the loop is.
//
// It is an estimate and nothing has checked it against a scope.  The
// error that matters is being too fast, so where it is uncertain it is
// biased slow: a line driven harder than it can carry fails in a way
// that looks like broken hardware.
//
#define SWD_LOOP_CYCLES		4
#define SWD_BIT_OVERHEAD	20

static void swd_set_clock(uint32_t hz)
{
	uint32_t cycles;

	if (!hz)
		return;

	cycles = clock_get_hz(clk_sys) / (2 * hz);
	cycles = cycles > SWD_BIT_OVERHEAD ? cycles - SWD_BIT_OVERHEAD : 1;
	cycles /= SWD_LOOP_CYCLES;

	swd_half_period = cycles ? (cycles > 4000 ? 4000 : cycles) : 1;
}

//
// Both directions follow one rule: data changes on the falling edge of
// SWCLK and is sampled on the rising edge.  So a bit we drive goes out
// while the clock is low and the target takes it on the way up, and a
// bit we read is sampled at the end of the low phase, after the target
// has had all of it to drive the line.
//
static void swd_write_bits(uint32_t bits, int n)
{
	gpio_set_dir(NRF54_SWDIO, GPIO_OUT);

	while (n--) {
		gpio_put(NRF54_SWDIO, bits & 1);
		bits >>= 1;
		swd_delay();
		gpio_put(NRF54_SWDCLK, 1);
		swd_delay();
		gpio_put(NRF54_SWDCLK, 0);
	}
}

static uint32_t swd_read_bits(int n)
{
	uint32_t bits = 0;

	gpio_set_dir(NRF54_SWDIO, GPIO_IN);

	for (int i = 0; i < n; i++) {
		swd_delay();
		bits |= (uint32_t)gpio_get(NRF54_SWDIO) << i;
		gpio_put(NRF54_SWDCLK, 1);
		swd_delay();
		gpio_put(NRF54_SWDCLK, 0);
	}
	return bits;
}

//
// One clock with nobody driving, which is how the line changes hands.
// The pull-up in swd_init() is what decides what an absent chip looks
// like: all ones, which is none of OK, WAIT or FAULT, so a board with
// no radio fails the probe rather than reading as noise.
//
static void swd_turnaround(void)
{
	swd_read_bits(1);
}

//
// A request names the register: which of the two ports, which
// direction, and two address bits.  These are the bit positions
// CMSIS-DAP uses for the same four things, so a transfer request off
// the wire is one of these with the match bits masked off.
//
#define SWD_AP		(1u << 0)
#define SWD_RNW		(1u << 1)
#define SWD_A2		(1u << 2)
#define SWD_A3		(1u << 3)

//
// OK, WAIT and FAULT are the target's, on the wire as it sends them.
// The fourth is ours: the transfer happened and the data came back
// corrupt, which is not something the target has a code for.
//
#define SWD_ACK_OK	1
#define SWD_ACK_WAIT	2
#define SWD_ACK_FAULT	4
#define SWD_ACK_PARITY	8

//
// Clocks with the line driven low, after a transfer.  Some targets need
// them to finish the previous access; the host says how many in
// DAP_TransferConfigure, and zero is the usual answer.
//
static uint8_t swd_idle_cycles;

static void swd_idle(void)
{
	if (swd_idle_cycles)
		swd_write_bits(0, swd_idle_cycles);
}

//
// One transfer.  The eight bits on the wire are start, APnDP, RnW,
// A[2], A[3], parity over those four, stop, park.
//
// On anything but OK the target does not drive a data phase, but the
// line still has to change hands, so the turnaround happens either way.
//
static int swd_transfer(unsigned int req, uint32_t wdata, uint32_t *rdata)
{
	unsigned int sel = req & (SWD_AP | SWD_RNW | SWD_A2 | SWD_A3);
	uint32_t packet = 1			// start
			| (sel << 1)
			| (__builtin_parity(sel) << 5)
			| (0u << 6)		// stop
			| (1u << 7);		// park
	uint32_t data, parity;
	int ack;

	swd_write_bits(packet, 8);
	swd_turnaround();
	ack = swd_read_bits(3);

	if (ack != SWD_ACK_OK) {
		swd_turnaround();
		swd_idle();
		return ack;
	}

	if (req & SWD_RNW) {
		data = swd_read_bits(32);
		parity = swd_read_bits(1);
		swd_turnaround();
		if (__builtin_parity(data) != parity) {
			swd_idle();
			return SWD_ACK_PARITY;
		}
		if (rdata)
			*rdata = data;
	} else {
		swd_turnaround();
		swd_write_bits(wdata, 32);
		swd_write_bits(__builtin_parity(wdata), 1);
	}

	swd_idle();
	return ack;
}

//
// The switch from JTAG to SWD, which is what an ADIv5 debug port wants
// before it will answer: at least fifty clocks with the line high, the
// sixteen-bit select sequence, fifty more, and two idle clocks low.
// The sequence is 0xE79E least-significant bit first - OpenOCD's
// swd_seq_jtag_to_swd, which stores it as the bytes 0x9e, 0xe7.
//
static void swd_connect_sequence(void)
{
	swd_write_bits(0xffffffff, 32);
	swd_write_bits(0xffffffff, 32);
	swd_write_bits(0xe79e, 16);
	swd_write_bits(0xffffffff, 32);
	swd_write_bits(0xffffffff, 32);
	swd_write_bits(0, 8);
}

//
// Reset is driven rather than assumed.  Nothing defines this pin until
// the RP2354 does: its pad comes out of reset with a pull-down against
// the nRF's pull-up, and that divider straddles the input threshold
// across the specified corners, so the part could come up either way.
// NOTES/radio.md has the resistances and the arithmetic.
//
// Hold it low, then let it go.
//
static void swd_init(void)
{
	gpio_init(NRF54_RESET);
	gpio_put(NRF54_RESET, 0);
	gpio_set_dir(NRF54_RESET, GPIO_OUT);

	gpio_init(NRF54_SWDCLK);
	gpio_put(NRF54_SWDCLK, 0);
	gpio_set_dir(NRF54_SWDCLK, GPIO_OUT);

	gpio_init(NRF54_SWDIO);
	gpio_pull_up(NRF54_SWDIO);
	gpio_set_dir(NRF54_SWDIO, GPIO_IN);
}

//
// Does anything answer on the SWD pins?
//
// The debug port's identity register answers on a part that has never
// been programmed, which is the whole point: it says the chip is
// present, powered and the right way round, and says nothing at all
// about whether it is running anything.  That second question is the
// UART's, and it has a different answer.
//
// Zero means no, and a board without a radio reaches nothing on these
// pins, so it gets that answer honestly rather than by being told.
//
static uint32_t nrf54_probe(void)
{
	uint32_t idcode;

	//
	// Drive the radio's CTS low before it boots.
	//
	// The nRF's UART runs with hardware flow control and its CTS is
	// active low, so it sends nothing while this pin is high.  Left
	// undriven the pin is the same divider as nRESET above, which a
	// typical part reads as high - so the radio would be mute from
	// its first instruction, and only the extreme corner would work.
	// A fault that varies between boards is worse than one that does
	// not.
	//
	// TODO: the UART peripheral takes this pin over once the link to
	// the radio is really implemented.
	//
	gpio_init(NRF54_RTS);
	gpio_put(NRF54_RTS, 0);
	gpio_set_dir(NRF54_RTS, GPIO_OUT);

	gpio_put(NRF54_RESET, 1);
	busy_wait_us_32(1000);

	swd_connect_sequence();

	if (swd_transfer(SWD_RNW, 0, &idcode) != SWD_ACK_OK)
		return 0;

	return idcode;
}

#endif /* NRF54_SWDIO */
#endif /* NRF54_SWD_H */
