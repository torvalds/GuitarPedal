#ifndef NRF54_UPDATE_H
#define NRF54_UPDATE_H

//
// Keeping the radio on the image this firmware carries.
//
// At boot the SWD probe has said whether there is a radio.  If there is,
// its first hello says what it is running: a SHA-256 the radio works out
// over its own RRAM at every boot.  When that is the hash of the image
// embedded here, nothing happens.  When it is not, or no hello comes, the
// image is written over SWD, the radio is reset, and its next hello has
// to match.  That is tried once per boot: a write that does not take is
// reported, and is tried again at the next boot.
//
// A write cut short is found the same way at the next boot, because the
// radio hashes what is actually there.  If what is there does not boot,
// the radio's debug port stays shut and the write starts with an erase of
// the whole chip, which takes its Bluetooth bonds with it - see rram.h.
//
// Only the radio's first start after the pedal's is checked.  An image
// put on with probe-rs afterwards is left alone until the pedal reboots.
//
// The write is done a block per pass of the main loop, so USB keeps
// being serviced through the seconds it takes, and the CMSIS-DAP
// interface is shut out while it runs.  Halting the radio, and erasing it
// when that is needed, each happen inside one pass.
//

#include "nRF54/app/src/link.h"

#ifdef NRF54_IMAGE

#include "nrf54_image.h"

//
// How long the radio gets to say hello, from the release of its reset:
// its own boot, the hash, and the pedal reaching its main loop.
//
#define NRF54_HELLO_MS		3000

//
// How much is written per pass of the main loop.  A block is a TAR block,
// or a part of one.
//
#define NRF54_UPDATE_BLOCK	256

//
// How many times one block, or the commit at the end, is tried.  A word
// corrupted on the wire is dropped by the radio and faults the transfer
// after it; the fault is cleared by then, and writing the same words
// again is harmless.
//
#define NRF54_BLOCK_TRIES	8

enum nrf54_update_state {
	NRF54_UPDATE_OFF,	// no radio
	NRF54_UPDATE_HELLO,	// waiting for the first hello
	NRF54_UPDATE_WRITING,
	NRF54_UPDATE_CHECK,	// written; waiting for the hello that says so
	NRF54_UPDATE_DONE,
};

static struct {
	enum nrf54_update_state state;
	uint32_t released;	// when the radio's reset was let go
	uint32_t since;		// when this state began
	uint32_t offset;	// how much has been written
	const char *why;	// why it is being written
	const char *result;

	// For the debug port, times in microseconds
	uint32_t hello_us;	// release to first hello
	uint32_t hash_us;	// the radio's own time to hash, first hello
	uint32_t write_us;
	uint32_t rewrites;	// blocks written again after a fault
	uint32_t check_us;	// reset to the hello after writing
	bool told;		// the reader on the debug port has been told
} nrf54_update;

static bool nrf54_update_busy(void)
{
	return nrf54_update.state == NRF54_UPDATE_WRITING;
}

//
// The radio answered on SWD and its reset has just been let go.
//
static void nrf54_update_start(void)
{
	nrf54_update.state = NRF54_UPDATE_HELLO;
	nrf54_update.released = nrf54_update.since = time_us_32();
}

static void nrf54_update_done(const char *result)
{
	nrf54_update.state = NRF54_UPDATE_DONE;
	nrf54_update.result = result;
	nrf54_update.told = false;
	report_status(result);
}

static void nrf54_update_write(const char *why)
{
	nrf54_update.state = NRF54_UPDATE_WRITING;
	nrf54_update.why = why;
	nrf54_update.offset = 0;
	nrf54_update.since = time_us_32();
	report_status("Updating the radio");
}

//
// A hello's payload, from after the link header.
//
static void nrf54_update_hello(const uint8_t *p, size_t len)
{
	uint32_t now = time_us_32();
	bool v1 = len >= LINK_HELLO_TEXT && p[LINK_HELLO_FORMAT] == LINK_HELLO_V1;
	bool match = v1 && !memcmp(p + LINK_HELLO_HASH, nrf54_image_sha256, 32);

	switch (nrf54_update.state) {
	case NRF54_UPDATE_HELLO:
		nrf54_update.hello_us = now - nrf54_update.released;
		if (v1)
			nrf54_update.hash_us = p[LINK_HELLO_HASH_US] |
					       p[LINK_HELLO_HASH_US + 1] << 8 |
					       p[LINK_HELLO_HASH_US + 2] << 16 |
					       (uint32_t)p[LINK_HELLO_HASH_US + 3] << 24;
		if (match)
			nrf54_update_done("The radio is up to date");
		else
			nrf54_update_write(v1 ? "its image differs"
					      : "its hello has no hash");
		break;
	case NRF54_UPDATE_CHECK:
		nrf54_update.check_us = now - nrf54_update.since;
		nrf54_update_done(match ? "The radio was updated"
					: "The radio update did not take");
		break;
	default:
		break;
	}
}

static void nrf54_update_reset(void)
{
	swd_flush();
	gpio_put(NRF54_RESET, 0);
	busy_wait_us_32(1000);
	gpio_put(NRF54_RESET, 1);
}

//
// Reset the radio after a failure too, so that it does not carry on from
// where it was halted, over what is now partly another image.
//
static void nrf54_update_failed(const char *result)
{
	nrf54_update.write_us = time_us_32() - nrf54_update.since;
	nrf54_rram_end();
	nrf54_update_reset();
	nrf54_update_done(result);
}

//
// The next block, with halting the radio before the first and committing
// and resetting it after the last.
//
static void nrf54_update_step(void)
{
	uint32_t off = nrf54_update.offset;
	uint32_t n = NRF54_UPDATE_BLOCK;
	int err = 0;

	if (!off) {
		swd_set_clock(SWD_HZ);	// a host may have asked for less
		if (nrf54_rram_begin()) {
			nrf54_update_failed("The radio could not be halted");
			return;
		}
	}

	if (NRF54_IMAGE_SIZE - off < n)
		n = NRF54_IMAGE_SIZE - off;
	for (int i = 0; i < NRF54_BLOCK_TRIES; i++) {
		nrf54_swd_err_clear();
		err = nrf54_rram_write(off, (const uint32_t *)(nrf54_image + off),
				       n / 4);
		if (!err)
			break;
		nrf54_update.rewrites++;
	}
	if (err) {
		nrf54_update_failed("The radio update failed writing");
		return;
	}
	nrf54_update.offset = off += n;
	if (off < NRF54_IMAGE_SIZE)
		return;

	for (int i = 0; i < NRF54_BLOCK_TRIES; i++) {
		nrf54_swd_err_clear();
		err = nrf54_rram_commit();
		if (!err)
			break;
	}
	if (err) {
		nrf54_update_failed("The radio update failed committing");
		return;
	}
	nrf54_rram_end();
	nrf54_update.write_us = time_us_32() - nrf54_update.since;
	nrf54_update_reset();

	nrf54_update.state = NRF54_UPDATE_CHECK;
	nrf54_update.since = time_us_32();
}

//
// What happened, on the debug port, once to each reader that connects.
//
static void nrf54_update_tell(void)
{
	if (!tud_cdc_connected()) {
		nrf54_update.told = false;
		return;
	}
	if (nrf54_update.told)
		return;
	nrf54_update.told = true;

	dbg_puts("radio: ");
	dbg_puts(nrf54_update.result);
	dbg_puts("\nradio: first hello ");
	if (nrf54_update.hello_us) {
		dbg_dec(nrf54_update.hello_us / 1000);
		dbg_puts(" ms after reset, hash ");
		dbg_dec(nrf54_update.hash_us / 1000);
		dbg_puts(" ms on the radio\n");
	} else {
		dbg_puts("never came\n");
	}
	if (!nrf54_update.why)
		return;

	dbg_puts("radio: written because ");
	dbg_puts(nrf54_update.why);
	dbg_puts(", ");
	dbg_dec(nrf54_update.offset);
	dbg_puts(" bytes in ");
	dbg_dec(nrf54_update.write_us / 1000);
	dbg_puts(" ms, ");
	dbg_dec(nrf54_update.rewrites);
	dbg_puts(" blocks written again\n");
	if (nrf54_erase_us) {
		dbg_puts("radio: its debug port was shut, so it was erased first, in ");
		dbg_dec(nrf54_erase_us / 1000);
		dbg_puts(" ms\n");
	}
	if (nrf54_swd_err.ack) {
		uint32_t cs = nrf54_swd_err.ctrl_stat;
		const uint8_t be[4] = { cs >> 24, cs >> 16, cs >> 8, cs };

		dbg_puts("radio: failed at ");
		dbg_puts(nrf54_swd_err.step);
		dbg_puts(", request ");
		dbg_dec(nrf54_swd_err.req);
		dbg_puts(", ack ");
		dbg_dec((uint32_t)nrf54_swd_err.ack);
		dbg_puts(", CTRL/STAT ");
		dbg_hex(be, 4);
		dbg_puts("\n");
	}
	if (nrf54_update.check_us) {
		dbg_puts("radio: hello after writing ");
		dbg_dec(nrf54_update.check_us / 1000);
		dbg_puts(" ms after reset\n");
	}
}

static void nrf54_update_poll(void)
{
	uint32_t waited = (time_us_32() - nrf54_update.since) / 1000;

	switch (nrf54_update.state) {
	case NRF54_UPDATE_HELLO:
		if (waited >= NRF54_HELLO_MS)
			nrf54_update_write("no hello came");
		break;
	case NRF54_UPDATE_WRITING:
		nrf54_update_step();
		break;
	case NRF54_UPDATE_CHECK:
		if (waited >= NRF54_HELLO_MS)
			nrf54_update_done("The radio did not start after updating");
		break;
	case NRF54_UPDATE_DONE:
		nrf54_update_tell();
		break;
	default:
		break;
	}
}

#else

static inline bool nrf54_update_busy(void) { return false; }
static inline void nrf54_update_start(void) { }
static inline void nrf54_update_hello(const uint8_t *p, size_t len) { }
static inline void nrf54_update_poll(void) { }

#endif /* NRF54_IMAGE */
#endif /* NRF54_UPDATE_H */
