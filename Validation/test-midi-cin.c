//
// Check the USB-MIDI Code Index Number tables in Software/midi.h.
//
// A lookup table is exactly the sort of thing that compiles cleanly and
// is wrong, and this one is only compiled at all when MIDI_HW is on -
// which it is not by default - so a mistake here would sit unnoticed.
//
// Build and run with 'make test-midi-cin && ./test-midi-cin'.
//
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "midi.h"

static int fails;
static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL %-28s got %d want %d\n", what, got, want);
		fails++;
	}
}

int main(void)
{
	// Channel voice: CIN is the top nibble, and the lengths are the
	// real MIDI data lengths plus the status byte.
	struct { uint8_t status; int cin, len; } v[] = {
		{ 0x80, 0x8, 3 },	// note off
		{ 0x90, 0x9, 3 },	// note on
		{ 0xA0, 0xA, 3 },	// poly key pressure - used to be dropped
		{ 0xB0, 0xB, 3 },	// control change
		{ 0xC0, 0xC, 2 },	// program change
		{ 0xD0, 0xD, 2 },	// channel pressure
		{ 0xE0, 0xE, 3 },	// pitch bend
		{ 0xF1, 0x2, 2 },	// MIDI time code
		{ 0xF2, 0x3, 3 },	// song position
		{ 0xF3, 0x2, 2 },	// song select
		{ 0xF6, 0x5, 1 },	// tune request
		{ 0xF8, 0xF, 1 },	// clock - used to be dropped
		{ 0xFA, 0xF, 1 },	// start
		{ 0xFC, 0xF, 1 },	// stop
		{ 0xFF, 0xF, 1 },	// reset
	};
	for (unsigned i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
		char buf[64];
		snprintf(buf, sizeof buf, "status %02x cin", v[i].status);
		chk(buf, midi_status_cin(v[i].status), v[i].cin);
		snprintf(buf, sizeof buf, "status %02x len", v[i].status);
		chk(buf, midi_cin_length(midi_status_cin(v[i].status)), v[i].len);
	}

	// Every channel-voice status must round-trip to a usable length,
	// regardless of channel nibble.
	for (int s = 0x80; s <= 0xEF; s++) {
		uint8_t cin = midi_status_cin(s);
		if (cin != (s >> 4)) { printf("FAIL cin nibble %02x\n", s); fails++; }
		if (midi_cin_length(cin) == 0) { printf("FAIL zero len %02x\n", s); fails++; }
	}

	// SysEx CINs carry data even though no single status byte maps to
	// them - this is what uart_midi_write() used to throw away.
	chk("sysex start/cont", midi_cin_length(0x4), 3);
	chk("sysex end 1", midi_cin_length(0x5), 1);
	chk("sysex end 2", midi_cin_length(0x6), 2);
	chk("sysex end 3", midi_cin_length(0x7), 3);

	// Reserved CINs carry nothing
	chk("reserved 0", midi_cin_length(0x0), 0);
	chk("reserved 1", midi_cin_length(0x1), 0);

	printf(fails ? "%d failures\n" : "all CIN checks pass\n", fails);
	return !!fails;
}
