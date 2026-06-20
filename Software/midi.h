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

bool handle_midi_packet(const uint8_t packet[4]);
void usb_midi_write(const uint8_t packet[4]);
void uart_midi_write(const uint8_t packet[4]);

static inline void send_midi_cc(uint8_t cc, uint8_t val)
{
	uint8_t packet[4] = { 0x0B, 0xB0, cc, val };
	usb_midi_write(packet);
	uart_midi_write(packet);
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
