#ifndef NRF54_DAP_H
#define NRF54_DAP_H

//
// CMSIS-DAP v2, so that programming the radio is somebody else's code.
//
// The RP2354 moves debug transactions between a bulk endpoint pair and
// two wires.  It never learns how the nRF54's non-volatile memory is
// written, what an image looks like, or how to verify one: probe-rs
// does all of that already.
//
// Its target list has no nRF54L10, so the invocation is
// `probe-rs --chip nRF54L15`.  Per its own target file the two parts
// share a debug interface and differ in how much memory they declare,
// and ours is a quarter of the smaller figure.
//
// Nothing here has run against hardware.
//

#ifdef NRF54_SWDIO

#include "pico/unique_id.h"

//
// One command per packet, one packet in flight, both reported in
// DAP_Info.
//
// Bulk USB is a byte stream with no message boundaries in it, so
// nothing in the bytes says where a command ends.  A command that fits
// in one 64-byte packet and a host that waits for each answer are the
// two halves of knowing anyway.  The receive fifo is one packet deep
// for the same reason - see tusb_config.h.
//
// A conforming host sends DAP_TransferAbort without waiting, so that
// one can arrive out of turn.  It is a no-op here in any case: a
// transfer runs to completion inside dap_poll(), so there is never one
// in flight to catch.
//
// The cost is that DAP_TransferBlock carries about fourteen words at a
// time.  Raising it means parsing a command's length out of its own
// contents, which is worth doing when the wire stops being the slow
// part, and is not yet.
//
#define DAP_PACKET_SIZE		64

#define ID_DAP_INFO		0x00
#define ID_DAP_HOST_STATUS	0x01
#define ID_DAP_CONNECT		0x02
#define ID_DAP_DISCONNECT	0x03
#define ID_DAP_TRANSFER_CONFIGURE 0x04
#define ID_DAP_TRANSFER		0x05
#define ID_DAP_TRANSFER_BLOCK	0x06
#define ID_DAP_TRANSFER_ABORT	0x07
#define ID_DAP_WRITE_ABORT	0x08
#define ID_DAP_DELAY		0x09
#define ID_DAP_RESET_TARGET	0x0a
#define ID_DAP_SWJ_PINS		0x10
#define ID_DAP_SWJ_CLOCK	0x11
#define ID_DAP_SWJ_SEQUENCE	0x12
#define ID_DAP_SWD_CONFIGURE	0x13

#define DAP_OK			0x00
#define DAP_ERROR		0xff

//
// The debug port's read buffer, at register 0x0c, which is where a
// posted read lands.
//
#define DP_RDBUFF		(SWD_RNW | SWD_A2 | SWD_A3)

#define DAP_WAIT_RETRY_MAX	1000

static uint16_t dap_wait_retry = 100;

//
// A target that says WAIT has not done the transfer, so the only
// answer is to ask again.  The host says how many times it is willing
// to wait, in DAP_TransferConfigure.
//
static int dap_retry(unsigned int req, uint32_t wdata, uint32_t *rdata)
{
	unsigned int tries = dap_wait_retry;
	int ack;

	do {
		ack = swd_transfer(req, wdata, rdata);
	} while (ack == SWD_ACK_WAIT && tries--);

	return ack;
}

//
// An access port read is posted: the transfer that asks for it returns
// the *previous* one's value, and the one asked for arrives next.
//
// Draining each one through RDBUFF straight away costs a second
// transaction per access port read, where carrying the pending value
// into the following transfer would not.  Take the cost.  Writing an
// image is the only sequence that has to be quick and it posts nothing,
// and keeping track of a value that spans two transfers is fiddly in
// proportion to how rarely it is exercised.
//
static int dap_read(unsigned int req, uint32_t *data)
{
	int ack = dap_retry(req | SWD_RNW, 0, data);

	if (ack == SWD_ACK_OK && (req & SWD_AP))
		ack = dap_retry(DP_RDBUFF, 0, data);

	return ack;
}

//
// The response byte carries the target's acknowledgement in its low
// three bits, and a bit of our own above them for a transfer that
// happened and came back corrupt.
//
#define DAP_TRANSFER_PROTOCOL_ERROR 0x08

static uint8_t dap_status(int ack)
{
	return ack == SWD_ACK_PARITY ? DAP_TRANSFER_PROTOCOL_ERROR : (uint8_t)ack;
}

static uint32_t dap_get32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void dap_put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

//
// The serial number is the board's, so two pedals plugged into one
// machine are distinguishable as debug probes and not only as pedals.
//
static uint32_t dap_info(uint8_t id, uint8_t *resp)
{
	static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
	const char *str;

	switch (id) {
	case 0x01:
		str = "Linus";
		break;
	case 0x02:
		str = PEDAL_BOARD_NAME " Pedal";
		break;
	case 0x03:
		pico_get_unique_board_id_string(serial, sizeof(serial));
		str = serial;
		break;
	case 0x04:
		str = "2.1.1";
		break;
	case 0xf0:
		// Capabilities: SWD, and no JTAG - there are two wires.
		resp[0] = 1;
		resp[1] = 1;
		return 2;
	case 0xfe:
		resp[0] = 1;
		resp[1] = 1;
		return 2;
	case 0xff:
		resp[0] = 2;
		resp[1] = DAP_PACKET_SIZE & 0xff;
		resp[2] = DAP_PACKET_SIZE >> 8;
		return 3;
	default:
		// An id we have nothing for returns an empty string,
		// which is a valid reply rather than an error.
		resp[0] = 0;
		return 1;
	}

	resp[0] = strlen(str) + 1;
	memcpy(resp + 1, str, resp[0]);
	return 1 + resp[0];
}

//
// Arbitrary clocks with arbitrary levels on SWDIO, which is how the
// host performs the connection sequence itself rather than asking us
// to know one.
//
static void dap_swj_sequence(const uint8_t *p, unsigned int bits)
{
	while (bits) {
		unsigned int n = bits > 8 ? 8 : bits;

		swd_write_bits(*p++, n);
		bits -= n;
	}
}

#define DAP_PIN_SWCLK		(1u << 0)
#define DAP_PIN_SWDIO		(1u << 1)
#define DAP_PIN_NRESET		(1u << 7)

static uint8_t dap_swj_pins(uint8_t value, uint8_t select)
{
	if (select & DAP_PIN_SWCLK)
		gpio_put(NRF54_SWDCLK, !!(value & DAP_PIN_SWCLK));
	if (select & DAP_PIN_SWDIO) {
		gpio_set_dir(NRF54_SWDIO, GPIO_OUT);
		gpio_put(NRF54_SWDIO, !!(value & DAP_PIN_SWDIO));
	}
	if (select & DAP_PIN_NRESET)
		gpio_put(NRF54_RESET, !!(value & DAP_PIN_NRESET));

	return (gpio_get(NRF54_SWDCLK) ? DAP_PIN_SWCLK : 0)
	     | (gpio_get(NRF54_SWDIO) ? DAP_PIN_SWDIO : 0)
	     | (gpio_get(NRF54_RESET) ? DAP_PIN_NRESET : 0);
}

//
// DAP_Transfer: a list of accesses, each with its own register, run
// until one of them does not answer OK.  The reply says how many were
// done, so a short count is how a failure is reported.
//
static uint32_t dap_transfer(const uint8_t *req, uint32_t len,
			     uint8_t *resp, uint32_t space)
{
	unsigned int count = req[1];	// req[0] is the DAP index, unused
	const uint8_t *p = req + 2;
	const uint8_t *end = req + len;
	uint8_t *out = resp + 2;
	const uint8_t *out_end = resp + space;
	unsigned int done = 0;
	int ack = SWD_ACK_OK;

	for (; done < count; done++) {
		unsigned int r;
		uint32_t data;

		if (p >= end)
			break;
		r = *p++;

		//
		// Value-match and match-mask ask the probe to poll a
		// register until it reads what the host wants.  Nothing
		// programming this part uses them, and a wrong answer
		// would look like a hung target.
		//
		if (r & 0x30) {
			ack = SWD_ACK_FAULT;
			break;
		}

		if (r & SWD_RNW) {
			//
			// Checked before the transfer, not after.  A
			// read of an auto-incrementing address moves
			// the target's address register, so doing one
			// whose result will not fit would step past a
			// word and never come back for it.
			//
			if (out + 4 > out_end)
				break;
			ack = dap_read(r, &data);
			if (ack != SWD_ACK_OK)
				break;
			dap_put32(out, data);
			out += 4;
		} else {
			if (p + 4 > end)
				break;
			data = dap_get32(p);
			p += 4;
			ack = dap_retry(r, data, NULL);
			if (ack != SWD_ACK_OK)
				break;
		}
	}

	resp[0] = done;
	resp[1] = dap_status(ack);
	return out - resp;
}

//
// DAP_TransferBlock: the same register, many times.  This is what
// writing an image is made of, so it is the one that wants the packet.
//
static uint32_t dap_transfer_block(const uint8_t *req, uint32_t len,
				   uint8_t *resp, uint32_t space)
{
	unsigned int count = req[1] | (req[2] << 8);
	unsigned int r = req[3];
	const uint8_t *p = req + 4;
	const uint8_t *end = req + len;
	uint8_t *out = resp + 3;
	const uint8_t *out_end = resp + space;
	unsigned int done = 0;
	int ack = SWD_ACK_OK;

	//
	// A block read asks for its count in two bytes and carries no
	// payload, so there is nothing in the request to run out of and
	// the response is the only thing bounding it.  Sixty-five
	// thousand reads would otherwise be written straight through the
	// end of the buffer and into whatever the audio core is using.
	//
	for (; done < count; done++) {
		uint32_t data;

		if (r & SWD_RNW) {
			if (out + 4 > out_end)
				break;
			ack = dap_read(r, &data);
			if (ack != SWD_ACK_OK)
				break;
			dap_put32(out, data);
			out += 4;
		} else {
			if (p + 4 > end)
				break;
			data = dap_get32(p);
			p += 4;
			ack = dap_retry(r, data, NULL);
			if (ack != SWD_ACK_OK)
				break;
		}
	}

	resp[0] = done;
	resp[1] = done >> 8;
	resp[2] = dap_status(ack);
	return out - resp;
}

//
// The reply always begins with the command byte it answers, which is
// what lets a host match the two up.
//
//
// The fewest bytes a command can arrive in and still be answered from
// what it actually contains.  Anything shorter would be answered out of
// whatever the last command left in the buffer, which is a wrong answer
// rather than no answer.
//
// These are the bytes each case below *reads*, not the command's
// specified length.  Requiring more than that is a conformance check
// nothing here needs, and it can only reject commands that would have
// worked - which is exactly what an off-by-one on DAP_SWJ_Clock did,
// refusing the first thing probe-rs sends.
//
static uint32_t dap_min_len(uint8_t cmd)
{
	switch (cmd) {
	case ID_DAP_INFO:
	case ID_DAP_CONNECT:
	case ID_DAP_SWD_CONFIGURE:
	case ID_DAP_SWJ_SEQUENCE:
		return 2;
	case ID_DAP_DELAY:
	case ID_DAP_SWJ_PINS:
	case ID_DAP_TRANSFER:
		return 3;
	case ID_DAP_TRANSFER_BLOCK:
	case ID_DAP_SWJ_CLOCK:
		return 5;
	case ID_DAP_TRANSFER_CONFIGURE:
	case ID_DAP_WRITE_ABORT:
		return 6;
	default:
		return 1;
	}
}

static uint32_t dap_process(const uint8_t *req, uint32_t len, uint8_t *resp)
{
	uint32_t clk;

	if (!len || len < dap_min_len(req[0])) {
		resp[0] = DAP_ERROR;
		return 1;
	}

	resp[0] = req[0];

	switch (req[0]) {
	case ID_DAP_INFO:
		return 1 + dap_info(req[1], resp + 1);

	case ID_DAP_HOST_STATUS:
		// A connect light and a running light, neither of which
		// this board has spare.
		resp[1] = 0;
		return 2;

	case ID_DAP_CONNECT:
		// Port 1 is SWD and port 0 means "whatever you have".
		// There is no JTAG on two wires, so both get SWD.
		resp[1] = (req[1] == 2) ? 0 : 1;
		return 2;

	case ID_DAP_DISCONNECT:
		resp[1] = DAP_OK;
		return 2;

	case ID_DAP_SWD_CONFIGURE:
		//
		// Byte 1 is the turnaround period in its low two bits and
		// a data-phase flag above them.  swd_transfer() does one
		// turnaround clock and no data phase, so anything else
		// has to be refused: accepting it would misframe every
		// transaction by a bit, and on a board nobody has run
		// that is indistinguishable from an unsoldered wire.
		//
		resp[1] = req[1] ? DAP_ERROR : DAP_OK;
		return 2;

	case ID_DAP_TRANSFER_CONFIGURE:
		swd_idle_cycles = req[1];
		dap_wait_retry = req[2] | (req[3] << 8);
		//
		// The host may ask for 65535 retries, and against a
		// target stuck in WAIT that is two seconds inside one
		// dap_poll() - long enough for the USB audio stream to
		// drop packets, because tud_task() runs in the same loop.
		// Cap it: a target that has not answered in a thousand
		// tries is not about to.
		//
		if (dap_wait_retry > DAP_WAIT_RETRY_MAX)
			dap_wait_retry = DAP_WAIT_RETRY_MAX;
		resp[1] = DAP_OK;
		return 2;

	case ID_DAP_TRANSFER:
		return 1 + dap_transfer(req + 1, len - 1, resp + 1,
					DAP_PACKET_SIZE - 1);

	case ID_DAP_TRANSFER_BLOCK:
		return 1 + dap_transfer_block(req + 1, len - 1, resp + 1,
					      DAP_PACKET_SIZE - 1);

	case ID_DAP_TRANSFER_ABORT:
		return 0;

	case ID_DAP_WRITE_ABORT:
		resp[1] = dap_retry(0, dap_get32(req + 2), NULL) == SWD_ACK_OK
			  ? DAP_OK : DAP_ERROR;
		return 2;

	case ID_DAP_DELAY:
		busy_wait_us_32(req[1] | (req[2] << 8));
		resp[1] = DAP_OK;
		return 2;

	case ID_DAP_RESET_TARGET:
		// No reset of our own beyond the pin, which the host can
		// drive through DAP_SWJ_Pins.
		resp[1] = DAP_OK;
		resp[2] = 0;
		return 3;

	case ID_DAP_SWJ_PINS:
		resp[1] = dap_swj_pins(req[1], req[2]);
		return 2;

	case ID_DAP_SWJ_CLOCK:
		clk = dap_get32(req + 1);
		swd_set_clock(clk);
		resp[1] = clk ? DAP_OK : DAP_ERROR;
		return 2;

	case ID_DAP_SWJ_SEQUENCE: {
		// A count of zero means 256 bits, and the host is taken at
		// its word no further than the bytes it actually sent.
		uint32_t bits = req[1] ? req[1] : 256;
		uint32_t have = (len - 2) * 8;

		dap_swj_sequence(req + 2, bits < have ? bits : have);
		resp[1] = DAP_OK;
		return 2;
	}

	default:
		resp[0] = DAP_ERROR;
		return 1;
	}
}

static void dap_poll(void)
{
	static uint8_t req[DAP_PACKET_SIZE], resp[DAP_PACKET_SIZE];
	uint32_t len, n;

	if (!tud_vendor_available())
		return;

	n = tud_vendor_read(req, sizeof(req));
	len = dap_process(req, n, resp);

	//
	// Every path above is bounded already.  If one ever stops being
	// so, sending the overrun would desync the session for good - the
	// host would read one reply as several and never line up again -
	// where dropping it costs a single command.
	//
	if (!len || len > sizeof(resp))
		return;

	tud_vendor_write(resp, len);
	tud_vendor_flush();
}

#endif /* NRF54_SWDIO */
#endif /* NRF54_DAP_H */
