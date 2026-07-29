//
// MIDI connection code
//

//
// MIDI Control Change (CC) constants
//
// Deep editing is SysEx - it names the effect and the pot explicitly, so
// nothing here needs to know about parameters.  What is left is
// performance control coming in and status going out.
//
//
// Global bypass in and out, and - on value 126 - reboot to the
// bootloader.
//
// This number is frozen, and not because it is a good one.  CC 20 is in
// the MSB half of the 14-bit controller range, so by the rule below it
// belongs up at 102 with the rest of what we invented.  But 126 on this
// controller is how you get an enclosed pedal into programming mode, and
// some enclosures have no exposed BOOTSEL to fall back on.  A pedal
// answers the number the firmware it is already running was built with,
// so moving it strands anything flashed before the move: the recovery
// path has to keep working on the old number, which means the old number
// is the only number.
//
#define MIDI_CC_GLOBAL_ENABLE    20

//
// Status out.
//
// Everything we made up lives in CC 102-119, which the spec leaves
// undefined.  Below 64 is the 14-bit convention, where CC n+32 is the
// LSB of CC n - so 32 is Bank Select LSB and anything in 0-31 can be
// read as an MSB waiting for its other half.  Where the standard already
// means what we mean, use the standard number instead: CC 7 is volume
// everywhere, and CC 11 is expression when that jack gets wired.
//
// The pedal has one LED and it can only say "something wants you".  The
// host can do better than that, so these say what.  Three CCs, split by
// what the answer is about rather than by which subsystem noticed:
//
// The global one carries what is not per-effect.  A count rather than a
// flag for the dropped samples, because "once" and "constantly" are
// different problems and the LED cannot tell you which - see status.h.
//
//	bits 0-4	samples dropped since the last report, to 31
//	bit 5		the output clipped
//	bit 6		effects[0], the front of the chain, wants attention
//
// The chain ones are one bit per routed effect in chain order, so the
// host can light the effect that is doing something instead of just
// reporting that something is.  Two of them because a CC value is seven
// bits and a chain can hold fourteen.
//
#define MIDI_CC_STATUS_GLOBAL    102
#define MIDI_CC_STATUS_CHAIN_LO  103
#define MIDI_CC_STATUS_CHAIN_HI  104

#define STATUS_DROPPED_MASK      0x1f
#define STATUS_CLIPPED           (1u << 5)
#define STATUS_FRONT_ATTN        (1u << 6)

// How many effects fit in one of the chain CCs
#define STATUS_CHAIN_BITS        7

//
// USB-MIDI 1.0 packs everything into four bytes: a cable number in the
// high nibble of the first byte - always zero here - and a Code Index
// Number in the low nibble, then up to three bytes of the message
// itself.  The CIN says what kind of message it is and, with it, how
// many of those three bytes are real.  0x0 and 0x1 are reserved and
// mean nothing to a host.
//
// Both directions of the hardware MIDI port need this, one to build a
// CIN and one to take it apart, so keep the two halves next to each
// other where they can be checked against one another.
//

// How many of the three data bytes a CIN actually carries
static inline int midi_cin_length(uint8_t cin)
{
	switch (cin) {
	case 0x5:	// single-byte system common, or SysEx ending on one
	case 0xF:	// single byte
		return 1;
	case 0x2:	// two-byte system common
	case 0x6:	// SysEx ending on two
	case 0xC:	// program change
	case 0xD:	// channel pressure
		return 2;
	case 0x3:	// three-byte system common
	case 0x4:	// SysEx start or continue
	case 0x7:	// SysEx ending on three
	case 0x8:	// note off
	case 0x9:	// note on
	case 0xA:	// poly key pressure
	case 0xB:	// control change
	case 0xE:	// pitch bend
		return 3;
	default:	// 0x0 and 0x1 are reserved
		return 0;
	}
}

// The CIN a status byte belongs in.  Not for SysEx, whose CIN depends
// on where in the stream the packet falls rather than on any one byte.
static inline uint8_t midi_status_cin(uint8_t status)
{
	if (status >= 0xF8)		// real time
		return 0xF;
	if (status >= 0xF0) {
		switch (status) {
		case 0xF1:		// MIDI time code
		case 0xF3:		// song select
			return 0x2;
		case 0xF2:		// song position
			return 0x3;
		default:		// tune request, and friends
			return 0x5;
		}
	}
	// Channel voice: the CIN is simply the top nibble
	return status >> 4;
}

//
// Turn a raw MIDI byte stream into USB-MIDI event packets.
//
// The UART sees the wire format, with running status and real-time bytes
// interspersed anywhere in another message.  USB-MIDI wants self-contained
// packets instead.  Keep the parser here with the CIN helpers: the mapping
// is part of parsing a byte stream, and putting it in a host-side test means
// the hardware MIDI path is exercised too.
//
struct midi_stream_parser {
	uint8_t bytes[3];
	int nr_bytes;
	int message_len;
	bool in_sysex;
};

static inline void midi_stream_reset(struct midi_stream_parser *parser)
{
	parser->nr_bytes = 0;
	parser->message_len = 0;
	parser->in_sysex = false;
}

static inline void midi_stream_packet(uint8_t packet[4], uint8_t cin,
	const uint8_t bytes[3], int nr_bytes)
{
	packet[0] = cin;
	packet[1] = nr_bytes > 0 ? bytes[0] : 0;
	packet[2] = nr_bytes > 1 ? bytes[1] : 0;
	packet[3] = nr_bytes > 2 ? bytes[2] : 0;
}

//
// Feed one byte of a raw MIDI stream.  Return true when it completes one
// USB-MIDI packet in 'packet'.  Real-time messages are emitted immediately
// without disturbing a partly received channel message or SysEx stream.
// System Reset is the exception: it also clears the parser state.
//
static inline bool midi_stream_read(struct midi_stream_parser *parser,
	uint8_t b, uint8_t packet[4])
{
	if (b >= 0xF8) {
		uint8_t byte[] = { b, 0, 0 };
		midi_stream_packet(packet, 0x0F, byte, 1);
		if (b == 0xFF)
			midi_stream_reset(parser);
		return true;
	}

	if (parser->in_sysex) {
		if (b == 0xF7) {
			parser->bytes[parser->nr_bytes++] = b;
			midi_stream_packet(packet,
				(uint8_t)(0x04 + parser->nr_bytes),
				parser->bytes, parser->nr_bytes);
			parser->in_sysex = false;
			parser->nr_bytes = 0;
			return true;
		}
		if (b < 0x80) {
			parser->bytes[parser->nr_bytes++] = b;
			if (parser->nr_bytes != 3)
				return false;
			midi_stream_packet(packet, 0x04, parser->bytes, 3);
			parser->nr_bytes = 0;
			return true;
		}

		// A non-real-time status byte abandons an unfinished SysEx.
		parser->in_sysex = false;
		parser->nr_bytes = 0;
	}

	if (b < 0x80) {
		if (!parser->message_len)
			return false;

		parser->bytes[parser->nr_bytes++] = b;
		if (parser->nr_bytes != parser->message_len)
			return false;

		midi_stream_packet(packet, midi_status_cin(parser->bytes[0]),
			parser->bytes, parser->nr_bytes);

		// Channel messages keep their status for MIDI running status.
		if (parser->bytes[0] < 0xF0)
			parser->nr_bytes = 1;
		else
			midi_stream_reset(parser);
		return true;
	}

	parser->nr_bytes = 0;
	parser->message_len = 0;
	if (b == 0xF0) {
		parser->bytes[parser->nr_bytes++] = b;
		parser->in_sysex = true;
		return false;
	}

	parser->bytes[parser->nr_bytes++] = b;
	if (b < 0xF0)
		parser->message_len = (b & 0xF0) == 0xC0 ||
			(b & 0xF0) == 0xD0 ? 2 : 3;
	else if (b == 0xF1 || b == 0xF3)
		parser->message_len = 2;
	else if (b == 0xF2)
		parser->message_len = 3;
	else if (b >= 0xF4 && b <= 0xF7)
		parser->message_len = 1;
	else
		return false;

	if (parser->message_len != 1)
		return false;

	midi_stream_packet(packet, midi_status_cin(b), parser->bytes, 1);
	midi_stream_reset(parser);
	return true;
}

bool handle_midi_packet(const uint8_t packet[4]);
void usb_midi_poll(void);
bool usb_midi_write(const uint8_t packet[4]);
bool usb_midi_write_nb(const uint8_t packet[4]);
void uart_midi_write(const uint8_t packet[4]);

static inline void send_midi_cc(uint8_t cc, uint8_t val)
{
	uint8_t packet[4] = { 0x0B, 0xB0, cc, val };
	usb_midi_write(packet);
	uart_midi_write(packet);
}

//
// The same, for something nobody is waiting on.
//
// Returns whether USB took it, so a caller that repeats itself anyway can
// simply not remember having sent it and say it again next time.  A host
// that is not reading fills the transmit fifo and every blocking write
// into it costs MIDI_TX_TIMEOUT_MS, which for anything periodic is a
// stall the pedal inflicts on itself for no reader's benefit.
//
// The UART is written either way and is not part of the answer: it is a
// ring that drops when full and never waits, so there is nothing to
// report and nothing to retry.
//
static inline bool send_midi_cc_nb(uint8_t cc, uint8_t val)
{
	uint8_t packet[4] = { 0x0B, 0xB0, cc, val };
	uart_midi_write(packet);
	return usb_midi_write_nb(packet);
}

static inline void send_midi_note_on(uint8_t ch, uint8_t note, uint8_t vel)
{
	uint8_t packet[4] = { 0x09, 0x90 | (ch & 0x0F), note, vel };
	usb_midi_write(packet);
	uart_midi_write(packet);
}

static inline void send_midi_note_off(uint8_t ch, uint8_t note, uint8_t vel)
{
	uint8_t packet[4] = { 0x08, 0x80 | (ch & 0x0F), note, vel };
	usb_midi_write(packet);
	uart_midi_write(packet);
}

static inline void send_midi_pitch_bend(uint8_t ch, int16_t bend)
{
	uint16_t val = bend + 8192;
	uint8_t packet[4] = { 0x0E, 0xE0 | (ch & 0x0F), val & 0x7F, (val >> 7) & 0x7F };
	usb_midi_write(packet);
	uart_midi_write(packet);
}

static inline void send_midi_channel_pressure(uint8_t ch, uint8_t pressure)
{
	uint8_t packet[4] = { 0x0D, 0xD0 | (ch & 0x0F), pressure & 0x7F, 0 };
	usb_midi_write(packet);
	uart_midi_write(packet);
}
