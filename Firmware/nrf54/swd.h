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
// Half a clock period.  The probe is a handful of transactions at boot
// and nothing waits on it, so this is slow on purpose: a bit-banged
// line that works is worth more than a fast one that is marginal, and
// the transfers that care about speed will not come through here.
//
#define SWD_HALF_PERIOD_NOPS	48

static inline void swd_delay(void)
{
	for (int i = 0; i < SWD_HALF_PERIOD_NOPS; i++)
		__asm__ volatile ("nop");
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
// like: all ones, which is not any of the three acknowledgements, so a
// board with no radio fails the probe rather than reading as noise.
//
static void swd_turnaround(void)
{
	swd_read_bits(1);
}

#define SWD_ACK_OK	1

//
// The eight-bit request: start, APnDP, RnW, A[2], A[3], parity over
// those four, stop, park.  'addr' is the register's byte offset, so
// A[3:2] come out of it.
//
static int swd_request(bool ap, bool rnw, uint8_t addr)
{
	unsigned int sel = ap | (rnw << 1) | (((addr >> 2) & 3) << 2);
	uint32_t req = 1			// start
		     | (sel << 1)
		     | (__builtin_parity(sel) << 5)
		     | (0u << 6)		// stop
		     | (1u << 7);		// park

	swd_write_bits(req, 8);
	swd_turnaround();
	return swd_read_bits(3);
}

static bool swd_read_reg(bool ap, uint8_t addr, uint32_t *out)
{
	uint32_t data, parity;

	if (swd_request(ap, true, addr) != SWD_ACK_OK)
		return false;

	data = swd_read_bits(32);
	parity = swd_read_bits(1);
	swd_turnaround();

	if (__builtin_parity(data) != parity)
		return false;

	*out = data;
	return true;
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

	if (!swd_read_reg(false, 0x00, &idcode))
		return 0;

	return idcode;
}

#endif /* NRF54_SWDIO */
#endif /* NRF54_SWD_H */
