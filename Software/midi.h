//
// MIDI connection code
//

// MIDI Control Change (CC) constants
#define MIDI_CC_POT_START        10 // CC 10-19 map to pots 0-9
#define MIDI_CC_GLOBAL_ENABLE    20
#define MIDI_CC_EFFECT_ENABLE    21
#define MIDI_CC_ACTIVE_POT       22

#define MIDI_CC_EFFECT_INTENSE   30
#define MIDI_CC_AUDIO_CLIPPING   31
#define MIDI_CC_CPU_LATENCY      32

extern int current_midi_effect_idx;

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
// the MIDI_HW-only path is exercised too.
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
void uart_midi_write(const uint8_t packet[4]);

static inline void send_midi_cc(uint8_t cc, uint8_t val)
{
	uint8_t packet[4] = { 0x0B, 0xB0, cc, val };
	usb_midi_write(packet);
	uart_midi_write(packet);
}

static inline void send_sysex_set_param(uint8_t eff_id, uint8_t pot_idx, uint8_t val)
{
	// F0 7D 03 <eff_id> <pot_idx> <val> F7
	uint8_t p1[4] = { 0x04, 0xF0, 0x7D, 0x03 };
	uint8_t p2[4] = { 0x04, eff_id, pot_idx, val };
	uint8_t p3[4] = { 0x05, 0xF7, 0, 0 };
	usb_midi_write(p1);
	usb_midi_write(p2);
	usb_midi_write(p3);
}

static inline void send_midi_pc(uint8_t pc)
{
	uint8_t packet[4] = { 0x0C, 0xC0, pc, 0 };
	usb_midi_write(packet);
	uart_midi_write(packet);
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
