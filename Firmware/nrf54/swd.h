#ifndef NRF54_SWD_H
#define NRF54_SWD_H

//
// SWD - ARM's two-wire Serial Wire Debug - to the nRF54L10.
//
// Three callers.  The boot probe below needs a single register read to
// answer "is there a radio on the other end of these wires".  A host
// programs the part over CMSIS-DAP (dap.h), and the ble board writes its
// embedded image itself (rram.h).  This file has the transactions; what
// to do with them is not here.
//
// The debug port is ADIv5, the older of ARM's two debug architectures,
// with the memory access port at index 0, and it needs no dormant-state
// wake-up before the ordinary JTAG-to-SWD switch.
//

#ifdef NRF54_SWDIO

//
// The wire is a PIO program (pio/swd.pio), on pio2 beside the WS2812s.
//
#define SWD_PIO		pio2
#define SWD_SM		PIO2_SWD_SM

//
// Four state machine cycles to a bit, and a whole number of system
// clocks to each of those, so every edge lands on the same system clock
// edge every time.  Rounded so the line is never faster than asked.
//
// SWD_HZ is both the rate and the most any caller gets, whatever a host
// asks for in DAP_SWJ_Clock.  Writing the whole radio image twice, each
// time changing every word, and reading it back after each, three boards
// had no error of any kind at 4.8, 6.4 or 7.68 MHz; this is well under
// half of that.  At 153.6 MHz it is 48 system clocks to a bit, a state
// machine divider of 12, and so 3.2 MHz exactly.
//
#define SWD_HZ		3200000

static void swd_set_clock(uint32_t hz)
{
	uint32_t div;

	if (!hz)
		return;
	if (hz > SWD_HZ)
		hz = SWD_HZ;

	div = (clock_get_hz(clk_sys) + 4 * hz - 1) / (4 * hz);
	if (div > 65535)
		div = 65535;
	pio_sm_set_clkdiv_int_frac(SWD_PIO, SWD_SM, div, 0);
}

//
// Stopped by dap_swj_pins(), which sets the pins itself and leaves them
// as they are until the next transfer starts the state machine again.
//
static bool swd_stopped;

static void swd_start(void)
{
	if (swd_stopped) {
		pio_sm_set_enabled(SWD_PIO, SWD_SM, true);
		swd_stopped = false;
	}
}

//
// Up to 32 bits each way, least significant first.  A write returns as
// soon as its two words are in the FIFO; a read waits for its bits, and
// so for everything queued ahead of it.
//
static void swd_write_bits(uint32_t bits, int n)
{
	swd_start();
	pio_sm_put_blocking(SWD_PIO, SWD_SM, n - 1);
	pio_sm_put_blocking(SWD_PIO, SWD_SM, bits);
}

static uint32_t swd_read_bits(int n)
{
	swd_start();
	pio_sm_put_blocking(SWD_PIO, SWD_SM, (n - 1) | SWD_PIO_READ);
	return pio_sm_get_blocking(SWD_PIO, SWD_SM) >> (32 - n);
}

//
// Wait until every bit queued has been clocked out, for whatever wants
// the pins in a known state afterwards.
//
static void swd_flush(void)
{
	uint32_t stalled = 1u << (PIO_FDEBUG_TXSTALL_LSB + SWD_SM);

	SWD_PIO->fdebug = stalled;
	while (!pio_sm_is_tx_fifo_empty(SWD_PIO, SWD_SM) ||
	       !(SWD_PIO->fdebug & stalled))
		;
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

	gpio_pull_up(NRF54_SWDIO);
	swd_program_init(SWD_PIO, SWD_SM, pio_add_program(SWD_PIO, &swd_program),
			 NRF54_SWDCLK, NRF54_SWDIO);
	swd_set_clock(SWD_HZ);
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

	gpio_put(NRF54_RESET, 1);
	busy_wait_us_32(1000);

	swd_connect_sequence();

	if (swd_transfer(SWD_RNW, 0, &idcode) != SWD_ACK_OK)
		return 0;

	return idcode;
}

#endif /* NRF54_SWDIO */
#endif /* NRF54_SWD_H */
