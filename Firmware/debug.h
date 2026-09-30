#ifndef DEBUG_H
#define DEBUG_H

#include <string.h>

//
// A serial port to the host, for the things that cannot be said over MIDI.
//
// The pedal has no console and is not getting one - status.h is the whole
// of what it says about itself, and that is deliberate.  This is for the
// *radio*, which has no way to speak at all: it shares one UART with the
// MIDI, so anything it says has to be valid MIDI, has to fit the 192 bytes
// of the pedal's inbound SysEx buffer, and arrives behind whatever else
// is queued.  A schema is tens of kilobytes of "whatever else".
//
// **Nothing may ever be gated on this.**  That is the rule and it is not
// a style preference: a channel that can hold anything up is one more way
// for one consumer to stop another, which is the defect it exists to help
// find.  So a write that does not fit is dropped and counted, and no
// caller is told, because there is nothing a caller could usefully do
// about it.
//
// It needs a host that has opened the port and raised DTR - which opening
// /dev/ttyACM0 does, so 'cat' is enough.  With nobody there the bytes are
// discarded rather than buffered, so what a reader sees when it connects
// is what happened after it connected, and never a fifo's worth of stale
// text from boot.
//
//	cat /dev/ttyACM0
//
// Lines end in a bare newline.  A CR as well would be correct for a
// terminal and wrong here: the tty's default ICRNL turns it into a second
// newline on the way in, so every line arrives double spaced.
//
// The audio core does not come here.  It has status.h, which is one
// relaxed store and nothing else, and check-audio.py would refuse the
// call this would take.
//

static struct {
	uint32_t dropped;	// bytes lost with somebody listening
} dbg;

//
// Bytes, or as many of them as fit.
//
static void dbg_write(const char *buf, size_t len)
{
	uint32_t took;

	if (!tud_cdc_connected())
		return;

	took = tud_cdc_write(buf, len);
	if (took < len)
		dbg.dropped += len - took;
}

static void dbg_puts(const char *s)
{
	dbg_write(s, strlen(s));
}

//
// A number, by hand rather than through printf - which nothing else in
// this firmware needs, and which would be several kilobytes to say how
// many bytes went missing.
//
static void dbg_dec(uint32_t v)
{
	char out[10];
	int n = 0;

	if (!v) {
		dbg_puts("0");
		return;
	}
	while (v && n < (int)sizeof(out)) {
		out[n++] = '0' + v % 10;
		v /= 10;
	}
	while (n--)
		dbg_write(&out[n], 1);
}

//
// Hand whatever has been written to USB.
//
// From the main loop, beside everything else that happens there.  It does
// not wait: a flush with the endpoint busy leaves the bytes in the fifo
// for the next pass, which is the same bargain as the rest of this.
//
static void dbg_task(void)
{
	tud_cdc_write_flush();
}

//
// Say what this is, once, when somebody starts listening.
//
// A reader that connects to a running pedal has no other way to find out
// what it is connected to, and the answer is two lines rather than a
// guess at the serial number in a dmesg.
//
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
	(void)itf;
	(void)rts;

	if (!dtr)
		return;

	dbg_puts("pedal " PEDAL_BOARD_NAME " " __DATE__ " " __TIME__ "\n");
	if (dbg.dropped) {
		dbg_puts("debug: ");
		dbg_dec(dbg.dropped);
		dbg_puts(" bytes dropped so far\n");
	}
}

#endif /* DEBUG_H */
