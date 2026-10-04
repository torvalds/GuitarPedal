#ifndef NRF54_RRAM_H
#define NRF54_RRAM_H

//
// Writing the radio's RRAM over SWD, with no host involved.
//
// What probe-rs does through dap.h, done here for the one job the ble
// board needs: halt the radio's core, write an image from address 0, and
// let it go.  Three layers, all on swd_transfer(): the debug port, the
// memory access port in front of the radio's bus, and the RRAM
// controller on that bus.
//
// The access port setup follows probe-rs's AHB3 access port, and the
// write sequence follows Zephyr's soc_flash_nrf_rram.c: enable writes
// with a 32-line write buffer, write the words, commit what is left in
// the buffer, wait for READY, disable writes.  RRAM has no erase.
//

#ifdef NRF54_IMAGE

//
// Registers, by their A[3:2] address, which is the same two bits
// SWD_A2 and SWD_A3 are - so an address is a request as it stands.
// DP_RDBUFF is dap.h's.
//
#define DP_DPIDR	0x0	// read
#define DP_ABORT	0x0	// write
#define DP_CTRL_STAT	0x4
#define DP_SELECT	0x8

#define AP_CSW		0x0
#define AP_TAR		0x4
#define AP_DRW		0xc

#define ABORT_CLEAR_ALL		0x1e
#define CTRL_STAT_PWRUPREQ	0x50000000	// CSYSPWRUPREQ | CDBGPWRUPREQ
#define CTRL_STAT_PWRUPACK	0xa0000000
#define CTRL_STAT_STICKYERR	0x00000020
#define CTRL_STAT_WDATAERR	0x00000080

#define CSW_DBGSWENABLE		(1u << 31)
#define CSW_HNONSEC		(1u << 30)
#define CSW_MASTER_DEBUG	(1u << 29)
#define CSW_CACHEABLE		(1u << 27)
#define CSW_PRIVILEGED		(1u << 25)
#define CSW_DATA		(1u << 24)
#define CSW_SPIDEN		(1u << 23)
#define CSW_DEVICEEN		(1u << 6)
#define CSW_ADDRINC_MASK	(3u << 4)
#define CSW_ADDRINC_SINGLE	(1u << 4)
#define CSW_SIZE_MASK		7u
#define CSW_SIZE_32		2u

#define DHCSR			0xe000edf0
#define DHCSR_KEY		0xa05f0000
#define DHCSR_C_DEBUGEN		(1u << 0)
#define DHCSR_C_HALT		(1u << 1)
#define DHCSR_S_HALT		(1u << 17)
#define DEMCR			0xe000edfc
#define DEMCR_VC_CORERESET	(1u << 0)

//
// Nordic's control access port, which answers when the memory access
// port does not.
//
#define CTRL_AP_SELECT		(2u << 24)
#define CTRL_AP_RESET		0x0
#define CTRL_AP_ERASEALL	0x4
#define CTRL_AP_ERASEALLSTATUS	0x8
#define CTRL_AP_RESET_NONE	0
#define CTRL_AP_RESET_HARD	2
#define ERASEALLSTATUS_READY	1
#define ERASEALLSTATUS_BUSY	2

#define RRAMC			0x5004b000	// the secure alias
#define RRAMC_COMMITWRITEBUF	(RRAMC + 0x008)
#define RRAMC_READY		(RRAMC + 0x400)
#define RRAMC_WRITEBUFEMPTY	(RRAMC + 0x418)
#define RRAMC_CONFIG		(RRAMC + 0x500)
#define RRAMC_CONFIG_WEN	(1u << 0)
#define RRAMC_CONFIG_BUF32	(32u << 8)

//
// TAR is only guaranteed to increment within a 1kB block, so a run of DRW
// writes restarts it at each boundary.
//
#define NRF54_TAR_BLOCK		1024

//
// The radio's bus stalls a write while RRAM is busy, and the access port
// answers WAIT until it is done.  A stall is one word's write, about 65
// microseconds, and no more than 30 WAITs in a row have been seen.
//
#define NRF54_WAIT_RETRIES	1000

//
// The first transfer to fail since this was last cleared, for the debug
// port: the request, the acknowledgement, and after a FAULT, what CTRL/STAT
// said about it.
//
static struct {
	const char *step;
	unsigned int req;
	int ack;
	uint32_t ctrl_stat;
} nrf54_swd_err;

static void nrf54_swd_err_clear(void)
{
	nrf54_swd_err.ack = 0;
	nrf54_swd_err.ctrl_stat = 0;
}

//
// After a FAULT the debug port answers FAULT to everything until its
// error flags are cleared, so they are cleared here, having been read.
//
static int nrf54_xfer(unsigned int req, uint32_t wdata, uint32_t *rdata)
{
	uint32_t ctrl_stat = 0;
	int ack = SWD_ACK_WAIT;

	for (int i = 0; i < NRF54_WAIT_RETRIES && ack == SWD_ACK_WAIT; i++)
		ack = swd_transfer(req, wdata, rdata);
	if (ack == SWD_ACK_OK)
		return 0;

	if (ack == SWD_ACK_FAULT) {
		swd_transfer(SWD_RNW | DP_CTRL_STAT, 0, &ctrl_stat);
		swd_transfer(DP_ABORT, ABORT_CLEAR_ALL, NULL);
	}
	if (!nrf54_swd_err.ack) {
		nrf54_swd_err.req = req;
		nrf54_swd_err.ack = ack;
		nrf54_swd_err.ctrl_stat = ctrl_stat;
	}
	return -1;
}

static int nrf54_dp_read(unsigned int reg, uint32_t *val)
{
	return nrf54_xfer(SWD_RNW | reg, 0, val);
}

static int nrf54_dp_write(unsigned int reg, uint32_t val)
{
	return nrf54_xfer(reg, val, NULL);
}

//
// An access port read is posted: what comes back is the result of the
// previous one, and RDBUFF holds this one's.
//
static int nrf54_ap_read(unsigned int reg, uint32_t *val)
{
	if (nrf54_xfer(SWD_AP | SWD_RNW | reg, 0, NULL))
		return -1;
	return nrf54_dp_read(DP_RDBUFF, val);
}

static int nrf54_ap_write(unsigned int reg, uint32_t val)
{
	return nrf54_xfer(SWD_AP | reg, val, NULL);
}

//
// Wait for the last access port write to reach the radio's bus.  A write
// is posted, and while RRAM stalls the bus it waits in the debug port; a
// read of CTRL/STAT in that window makes the debug port drop it and set
// WDATAERR.  A read of RDBUFF is held off with WAIT until it has gone.
//
static int nrf54_ap_drain(void)
{
	uint32_t val;

	return nrf54_dp_read(DP_RDBUFF, &val);
}

static int nrf54_mem_write(uint32_t addr, uint32_t val)
{
	if (nrf54_ap_write(AP_TAR, addr))
		return -1;
	return nrf54_ap_write(AP_DRW, val);
}

static int nrf54_mem_read(uint32_t addr, uint32_t *val)
{
	if (nrf54_ap_write(AP_TAR, addr))
		return -1;
	return nrf54_ap_read(AP_DRW, val);
}

//
// Wait for a register to have the given bits set, up to 'tries' reads.
//
static int nrf54_mem_wait(uint32_t addr, uint32_t bits, int tries)
{
	uint32_t val;

	while (tries--) {
		if (nrf54_mem_read(addr, &val))
			return -1;
		if ((val & bits) == bits)
			return 0;
	}
	return -1;
}

//
// How many times to try halting a radio that keeps resetting.
//
#define NRF54_HALT_TRIES	100

static bool nrf54_ap_closed;
static uint32_t nrf54_erase_us;	// how long an erase took, or zero

//
// Connect, power up the debug domain and set up the access port.  The
// line is reset first, because the radio may have reset since it was
// last used, which powers its debug domain down.
//
static int nrf54_attach(void)
{
	uint32_t val, csw;
	int i;

	nrf54_swd_err.step = "connect";
	swd_connect_sequence();
	if (nrf54_dp_read(DP_DPIDR, &val))
		return -1;
	if (nrf54_dp_write(DP_ABORT, ABORT_CLEAR_ALL) ||
	    nrf54_dp_write(DP_SELECT, 0) ||
	    nrf54_dp_write(DP_CTRL_STAT, CTRL_STAT_PWRUPREQ))
		return -1;
	for (i = 0; i < 100; i++) {
		if (nrf54_dp_read(DP_CTRL_STAT, &val))
			return -1;
		if ((val & CTRL_STAT_PWRUPACK) == CTRL_STAT_PWRUPACK)
			break;
	}
	if (i == 100)
		return -1;

	if (nrf54_ap_read(AP_CSW, &csw))
		return -1;
	nrf54_ap_closed = !(csw & CSW_DEVICEEN);
	if (nrf54_ap_closed)
		return -1;
	csw &= ~(CSW_HNONSEC | CSW_ADDRINC_MASK | CSW_SIZE_MASK);
	csw |= CSW_DBGSWENABLE | CSW_MASTER_DEBUG | CSW_CACHEABLE |
	       CSW_PRIVILEGED | CSW_DATA | CSW_ADDRINC_SINGLE | CSW_SIZE_32;
	if (!(csw & CSW_SPIDEN))
		csw |= CSW_HNONSEC;
	return nrf54_ap_write(AP_CSW, csw);
}

//
// Halt the core, and halt it on reset too, so that a reset that still
// happens leaves it stopped at its first instruction rather than running.
//
static int nrf54_halt(void)
{
	nrf54_swd_err.step = "halt";
	if (nrf54_mem_write(DEMCR, DEMCR_VC_CORERESET) ||
	    nrf54_mem_write(DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT))
		return -1;
	return nrf54_mem_wait(DHCSR, DHCSR_S_HALT, 100);
}

//
// Erase all of RRAM through the control access port, which opens the
// memory access port again.  The nRF54L closes that port at every reset
// and the image's startup code opens it, so an image that does not get
// that far leaves no other way in.  The Bluetooth bonds go with it.
//
static int nrf54_erase_all(void)
{
	uint32_t status = ERASEALLSTATUS_BUSY;
	uint32_t start = time_us_32();

	nrf54_swd_err.step = "erase";
	if (nrf54_dp_write(DP_SELECT, CTRL_AP_SELECT) ||
	    nrf54_ap_write(CTRL_AP_ERASEALL, 1))
		return -1;
	while (status == ERASEALLSTATUS_BUSY) {
		if (time_us_32() - start > 5000000)
			return -1;
		busy_wait_us_32(1000);
		if (nrf54_ap_read(CTRL_AP_ERASEALLSTATUS, &status))
			return -1;
	}
	if (status != ERASEALLSTATUS_READY)
		return -1;
	if (nrf54_ap_write(CTRL_AP_RESET, CTRL_AP_RESET_HARD) ||
	    nrf54_ap_write(CTRL_AP_RESET, CTRL_AP_RESET_NONE))
		return -1;
	nrf54_erase_us = time_us_32() - start;
	return 0;
}

//
// Attach, halt and enable writes.  A radio with a broken image may be
// resetting over and over, and an access during a reset faults, so the
// whole of it is tried until it takes - erasing first, once, if the
// access port is shut.
//
static int nrf54_rram_begin(void)
{
	int i;

	nrf54_erase_us = 0;
	for (i = 0; i < NRF54_HALT_TRIES; i++) {
		nrf54_swd_err_clear();
		if (!nrf54_attach() && !nrf54_halt())
			break;
		if (nrf54_ap_closed && !nrf54_erase_us && nrf54_erase_all())
			return -1;
		busy_wait_us_32(100);
	}
	if (i == NRF54_HALT_TRIES)
		return -1;

	nrf54_swd_err.step = "write";
	return nrf54_mem_write(RRAMC_CONFIG, RRAMC_CONFIG_WEN |
					     RRAMC_CONFIG_BUF32);
}

//
// Words to RRAM at 'addr', which with 'n' must stay inside one TAR
// block.  Checked once at the end rather than per word: a write that
// fails on the radio's bus sets STICKYERR and one that arrived corrupt
// sets WDATAERR, and every access after either fails too, so the next
// one that reports success means all of them did.
//
static int nrf54_rram_write(uint32_t addr, const uint32_t *words, unsigned int n)
{
	uint32_t val;

	if (nrf54_ap_write(AP_TAR, addr))
		return -1;
	for (unsigned int i = 0; i < n; i++)
		if (nrf54_ap_write(AP_DRW, words[i]))
			return -1;
	if (nrf54_ap_drain() || nrf54_dp_read(DP_CTRL_STAT, &val))
		return -1;
	if (!(val & (CTRL_STAT_STICKYERR | CTRL_STAT_WDATAERR)))
		return 0;

	nrf54_dp_write(DP_ABORT, ABORT_CLEAR_ALL);
	if (!nrf54_swd_err.ack) {
		nrf54_swd_err.req = SWD_AP | AP_DRW;
		nrf54_swd_err.ack = SWD_ACK_FAULT;
		nrf54_swd_err.ctrl_stat = val;
	}
	return -1;
}

//
// Commit what is still in the write buffer and wait for RRAM to finish.
//
static int nrf54_rram_commit(void)
{
	uint32_t val;

	nrf54_swd_err.step = "commit";
	if (nrf54_mem_read(RRAMC_WRITEBUFEMPTY, &val))
		return -1;
	if (!(val & 1) && nrf54_mem_write(RRAMC_COMMITWRITEBUF, 1))
		return -1;
	return nrf54_mem_wait(RRAMC_READY, 1, 1000);
}

//
// Disable writes, let the core go and power the debug domain down, as
// far as the radio still answers.  The caller resets the radio.
//
static void nrf54_rram_end(void)
{
	nrf54_mem_write(RRAMC_CONFIG, 0);
	nrf54_mem_write(DEMCR, 0);
	nrf54_mem_write(DHCSR, DHCSR_KEY);
	nrf54_ap_drain();
	nrf54_dp_write(DP_CTRL_STAT, 0);
}

#endif /* NRF54_IMAGE */
#endif /* NRF54_RRAM_H */
