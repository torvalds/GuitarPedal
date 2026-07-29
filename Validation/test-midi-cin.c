//
// Check the USB-MIDI Code Index Number helpers and byte-stream parser in
// Firmware/midi/midi.h.
//
// A lookup table or parser is exactly the sort of thing that compiles cleanly
// and is wrong.  Keep host-side coverage for the firmware's hardware MIDI
// path so mistakes do not require a pedal and TRS adapter to find.
//
// Build and run with 'make test-midi-cin && ./test-midi-cin'.
//
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "midi/midi.h"

static int fails;
static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL %-28s got %d want %d\n", what, got, want);
		fails++;
	}
}

static void chk_packet(const char *what, const uint8_t packet[4],
	uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
	if (packet[0] != cin || packet[1] != b1 || packet[2] != b2 ||
	    packet[3] != b3) {
		printf("FAIL %-28s got %02x %02x %02x %02x want %02x %02x %02x %02x\n",
			what, packet[0], packet[1], packet[2], packet[3],
			cin, b1, b2, b3);
		fails++;
	}
}

static void chk_stream_packet(struct midi_stream_parser *parser,
	uint8_t b, const char *what, uint8_t cin, uint8_t b1, uint8_t b2,
	uint8_t b3)
{
	uint8_t packet[4];

	if (!midi_stream_read(parser, b, packet)) {
		printf("FAIL %-28s no packet\n", what);
		fails++;
		return;
	}
	chk_packet(what, packet, cin, b1, b2, b3);
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

	// The DIN port receives a byte stream, not USB-MIDI packets. Check
	// that the stream parser preserves running status, emits real-time
	// messages in the middle of another message, and packs SysEx endings
	// with the right number of bytes.
	struct midi_stream_parser parser;
	midi_stream_reset(&parser);
	chk("note status", midi_stream_read(&parser, 0x90, (uint8_t[4]) { 0 }), false);
	chk("note data 1", midi_stream_read(&parser, 0x3C, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0xF8, "clock between note bytes", 0x0F, 0xF8, 0, 0);
	chk_stream_packet(&parser, 0x40, "note after clock", 0x09, 0x90, 0x3C, 0x40);
	chk("running data 1", midi_stream_read(&parser, 0x3D, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0x41, "running note", 0x09, 0x90, 0x3D, 0x41);

	midi_stream_reset(&parser);
	chk("program status", midi_stream_read(&parser, 0xC0, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0x05, "program change", 0x0C, 0xC0, 0x05, 0);
	chk_stream_packet(&parser, 0x06, "running program", 0x0C, 0xC0, 0x06, 0);

	// System Reset is real-time, but unlike the other real-time messages
	// it resets receivers to power-up state and therefore cancels both
	// a message in progress and running status.
	midi_stream_reset(&parser);
	chk("reset note status", midi_stream_read(&parser, 0x90, (uint8_t[4]) { 0 }), false);
	chk("reset note data 1", midi_stream_read(&parser, 0x3C, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0xFF, "system reset", 0x0F, 0xFF, 0, 0);
	chk("data after reset", midi_stream_read(&parser, 0x40, (uint8_t[4]) { 0 }), false);

	midi_stream_reset(&parser);
	chk("song position status", midi_stream_read(&parser, 0xF2, (uint8_t[4]) { 0 }), false);
	chk("song position data 1", midi_stream_read(&parser, 0x01, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0x02, "song position", 0x03, 0xF2, 0x01, 0x02);
	chk_stream_packet(&parser, 0xF6, "tune request", 0x05, 0xF6, 0, 0);
	chk_stream_packet(&parser, 0xF7, "standalone EOX", 0x05, 0xF7, 0, 0);

	midi_stream_reset(&parser);
	chk("sysex f0", midi_stream_read(&parser, 0xF0, (uint8_t[4]) { 0 }), false);
	chk("sysex byte 1", midi_stream_read(&parser, 0x7D, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0xF8, "clock inside sysex", 0x0F, 0xF8, 0, 0);
	chk_stream_packet(&parser, 0x03, "sysex start", 0x04, 0xF0, 0x7D, 0x03);
	chk("sysex byte 2", midi_stream_read(&parser, 0x12, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0xF7, "sysex end 2", 0x06, 0x12, 0xF7, 0);

	midi_stream_reset(&parser);
	chk("short sysex f0", midi_stream_read(&parser, 0xF0, (uint8_t[4]) { 0 }), false);
	chk("short sysex byte", midi_stream_read(&parser, 0x7D, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0xF7, "sysex end 3", 0x07, 0xF0, 0x7D, 0xF7);

	midi_stream_reset(&parser);
	chk("long sysex f0", midi_stream_read(&parser, 0xF0, (uint8_t[4]) { 0 }), false);
	chk("long sysex byte 1", midi_stream_read(&parser, 0x01, (uint8_t[4]) { 0 }), false);
	chk_stream_packet(&parser, 0x02, "long sysex start", 0x04, 0xF0, 0x01, 0x02);
	chk_stream_packet(&parser, 0xF7, "sysex end 1", 0x05, 0xF7, 0, 0);

	printf(fails ? "%d failures\n" : "all MIDI checks pass\n", fails);
	return !!fails;
}
