#ifndef MIDI_SYSEX_H
#define MIDI_SYSEX_H

//
// What the pedal says over MIDI, and what it does about what it hears.
//
// The layer underneath is midi/midi.h for the numbers and the four-byte
// packet, midi/tx.h for the queue those packets are drained from, and
// midi/uart.h for the other wire.  This is the part that knows what a
// pedal is: that 0x0A means "say what you are", that a state dump ends
// with the routing so the app can tell when it has all of it, that a
// scene has to be saved before it can be loaded.
//
// It is a header rather than a .c file because this firmware is one
// translation unit - see midi/tx.h - and because everything here reaches
// into effects[], the routing bitmap, the rule tables and the save area.
// A separate object file would be an extern for each of those.
//
// Include order is program order here, so this sits after effect-state.h
// (it calls the setters), after scene.h (it saves and loads), and after
// hardware.h (the identity reply reports what the probe found), and
// before ui.h, which calls sysex_echo_pot() and sysex_send_bindings().
//
// Two edges do not fit that order, and both are declared ahead of here
// rather than reshuffling the list.  handle_midi_packet() is declared in
// midi/midi.h, so that midi/uart.h and usb-device.c can both reach it
// from where they are.  sysex_echo_pot() is declared in exp.h, which is
// before this and has something to say while a treadle is being
// calibrated - and only there, because it is static and usb-device.c has
// no business hearing about it.
//

#include "midi_schema.h"
#include "tx.h"

//
// A reply is built whole and then queued, or it is not queued at all.
//
// This used to be a packetiser that pushed at USB as it went, and the
// first packet that would not go abandoned the rest of the message: every
// later write turned into a no-op and no 0xF7 went out.  Abandoning was
// better than truncating, because Web MIDI only delivers SysEx messages
// that were terminated - so a message that never ends is one the app
// simply never sees and can ask for again, while a truncated one with an
// 0xF7 stuck on the end arrives looking complete and gets parsed as
// garbage.  Half a JSON schema, say.
//
// All of that survives, and is now the ordinary case rather than the
// failure: midi_tx_commit() either publishes the whole reply or rewinds
// as though it had never been built.  What has gone is the *reason* a
// reply used to fail, which was a transmit fifo that had no room this
// millisecond.  That is not a failure, it is a wait, and the queue is
// somewhere to wait.
//
//
// Who sent the message being handled now: USB, or a Bluetooth peer, or -
// outside the handling of any message - everyone, which is who whatever the
// pedal says of its own accord is for.  handle_midi_packet_from() sets it.
//
static struct midi_dest midi_from = { MIDI_TO_USB | MIDI_TO_RADIO, 0 };

//
// A reply to whoever sent the message being handled, which for one built
// later from a flag is whoever set the flag - see the reply flags below.
//
static void sysex_tx_start(void)
{
	midi_tx_start(midi_from);
}

static void sysex_tx_start_to(struct midi_dest dest)
{
	midi_tx_start(dest);
}

//
// True when the reply is queued, which is the caller's cue to stop asking
// for it.  False means the queue is busy, not that anything went wrong -
// leave the request flag set and it will be built again next time round
// the main loop.
//
static bool sysex_tx_finish(const char *sent)
{
	if (!midi_tx_commit())
		return false;
	report_info(sent);
	return true;
}

static void sysex_stream_write(const uint8_t *buffer, size_t len)
{
	midi_tx_bytes(buffer, len);
}


static void sysex_write_str(const char *str)
{
	sysex_stream_write((const uint8_t *)str, strlen(str));
}

//
// A number, for the JSON below.  Hand-rolled for the same reason
// eeprom.h rolls its own: snprintf() drags newlib's float conversion in
// behind it, and scripts/check-float.py exists to refuse exactly that.
//
static void sysex_write_num(uint32_t val)
{
	char buf[11];
	unsigned int n = sizeof(buf);

	buf[--n] = 0;
	do {
		buf[--n] = '0' + val % 10;
		val /= 10;
	} while (val);

	sysex_write_str(buf + n);
}

//
// Identity, as JSON.
//
// Two kinds of thing come back from the pedal and they want opposite
// encodings.  This one is asked once, when the app connects, so a couple
// of hundred bytes cost nothing and being self-describing means a field
// can be added later without either side agreeing a version first - the
// same bargain the schema already makes.  Anything *polled* is the other
// case and should be packed bytes instead.
//
// Measured on the wire: 208 bytes, one SysEx message.  So "a couple of
// hundred" is exactly right, which is worth writing down because a first
// attempt at measuring it said 1918 - the pedal streams its status CCs
// continuously, so a capture window long enough to be sure of catching a
// reply collects a second of those alongside it, and the inflation
// scales with the window rather than with the reply.  Any figure taken
// off a raw byte count has that in it.
//
struct midi_dest send_identity_tx;
static void sysex_send_identity(void)
{
	if (!send_identity_tx.to)
		return;

	//
	// One reply at a time.
	//
	// Not a queue depth limit - it is about the cost of *building*.  A
	// sender that cannot commit leaves its request flag set and is
	// called again next pass, and without this it would serialise the
	// whole reply again every time round the main loop for as long as
	// the queue stayed busy.  For a state dump that means walking every
	// effect and both rule tables, hundreds of times, to throw all of
	// it away.
	//
	// It also means the payload ring only ever holds one reply, which
	// is what lets it be sized for the largest single one rather than
	// for some guess at how many might pile up.
	//
	if (midi_tx_busy())
		return;

	static const uint8_t sysex_identity_header[] = { 0xF0, 0x7D, 0x0A };
	static const uint8_t sysex_identity_trailer[] = { 0xF7 };

	sysex_tx_start_to(send_identity_tx);
	sysex_stream_write(sysex_identity_header, sizeof(sysex_identity_header));

	//
	// The build stamp is what answers "did I actually reflash?", which
	// is the question that gets asked in anger.  It only moves when
	// blink.c is recompiled, which is exactly when the binary changed.
	//
	sysex_write_str("{\"build\":\"" __DATE__ " " __TIME__ "\"");

	//
	// Which schema 0x01 would send, so an app holding a copy with this
	// hash can skip asking for the whole schema.
	//
	sysex_write_str(",\"schema\":\"" MIDI_SCHEMA_HASH "\"");

	//
	// Which pedal this is: the chip's unique id, the same string as the
	// USB serial.  A pedal reached over USB and over Bluetooth gives the
	// same answer to both, and nothing else in either says so.
	//
	{
		char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

		pico_get_unique_board_id_string(id, sizeof(id));
		sysex_write_str(",\"serial\":\"");
		sysex_write_str(id);
		sysex_write_str("\"");
	}
	//
	// How many scenes there are, and which of them have ever been
	// saved.  The count used to depend on which eeprom was fitted and
	// needed a paragraph explaining how it had been guessed; it is a
	// build constant now, because every board has the same 2MB of
	// flash on the die.
	//
#ifdef PEDAL_BRANCH
	sysex_write_str(",\"branch\":\"" PEDAL_BRANCH "\"");
#endif
	sysex_write_str(",\"scenes\":");
	sysex_write_num(MAX_SCENES);
	sysex_write_str(",\"populated\":");
	sysex_write_num(populated_scenes());
	sysex_write_str(",\"midi_hw\":");
	sysex_write_str(MIDI_HW ? "true" : "false");
	sysex_write_str(",\"found\":{\"i2c_codec\":");
	sysex_write_str(hardware.i2c_codec ? "true" : "false");
	//
	// ...and what that means on this board, because the same bit means
	// mono-against-stereo on one family and DC-against-AC-coupled on
	// another.  A named key, so appending one is safe here in a way
	// 315 says it is not in the telemetry block.
	//
	sysex_write_str(",\"codec\":\"");
	sysex_write_str(hardware.i2c_codec ? CODEC_I2C_DESC : CODEC_STRAPPED_DESC);
	sysex_write_str("\"");
	sysex_write_str(",\"legacy_screen\":");
	sysex_write_str(hardware.legacy_screen ? "true" : "false");
#ifdef NRF54_SWDIO
	//
	// The debug port's id rather than a bit, because it is what was
	// read and the reader can decide what it means.  Absent on a board
	// with nowhere for a radio to be, which is not the same as zero.
	//
	sysex_write_str(",\"radio_idcode\":");
	sysex_write_num(hardware.radio);
#endif
	sysex_write_str("}");

	//
	// And the answers to what the effects asked for, by the name they
	// asked under.  The schema says which effect needs which, because
	// that is a fact about the build; this says what this board turned
	// out to have, which only running on it can settle.
	//
	// Named rather than a list of absent effect ids, so that two
	// effects wanting the same thing cost one answer - and so the app
	// can say which piece of hardware is missing rather than only that
	// something is.
	//
	sysex_write_str(",\"have\":{");
	{
		const char *sep = "";

#define HW_NAME(text, what) do {					\
		sysex_write_str(sep);					\
		sysex_write_str("\"" text "\":");			\
		sysex_write_str((HAVE_##what) ? "true" : "false");	\
		sep = ",";						\
	} while (0);
		EFFECT_HW_NAMES
#undef HW_NAME
	}
	sysex_write_str("}");

	//
	// The controls this board has, so the app draws what is there
	// rather than a list of its own.  Which ones exist depends on the
	// board, and shortly on what is in the expression jack, so it
	// belongs in this reply rather than in the schema - that one is a
	// const string in flash and cannot know either.
	//
	sysex_write_str(",\"controls\":[");
	for (unsigned int i = 0, n = 0; i < NR_CONTROLS; i++) {
		if (!controls[i].name)
			continue;
#ifdef EXP_TIP_GPIO
		if (i >= CTRL_EXP_TIP_TAP && !exp_control_offered(i))
			continue;
#endif
		sysex_write_str(n++ ? ",{\"id\":" : "{\"id\":");
		sysex_write_num(i);
		sysex_write_str(",\"name\":\"");
		sysex_write_str(controls[i].name);
		sysex_write_str("\",\"kind\":\"");
		sysex_write_str(controls[i].kind);
		sysex_write_str("\"}");
	}
	sysex_write_str("]");

	//
	// What is in the save area, read now.  Nothing needs this to
	// run; it is here so that slots planted with picotool can be
	// asked about from a shell, which is the whole test rig for the
	// format - 'marked' counts what carried a marker and 'valid'
	// what also survived its hash, so a deliberately corrupted
	// signature shows up as the difference between the two.
	//
	struct save_scan found;

	save_scan(&found);

	sysex_write_str(",\"save\":{\"slots\":");
	sysex_write_num(SAVE_SLOT_COUNT);
	sysex_write_str(",\"marked\":");
	sysex_write_num(found.marked);
	sysex_write_str(",\"valid\":");
	sysex_write_num(found.valid);
	sysex_write_str(",\"keys\":");
	sysex_write_num(found.keys);
	sysex_write_str(",\"newest\":");
	sysex_write_num(found.newest);
	sysex_write_str("}}");

	sysex_stream_write(sysex_identity_trailer, sizeof(sysex_identity_trailer));
	if (sysex_tx_finish("Sent identity"))
		send_identity_tx = MIDI_DEST_NONE;
}

//
// Telemetry: what the pedal can see about its own signal.
//
// Packed bytes rather than the JSON the identity reply uses, because this
// is the polled one - asked several times a second while somebody watches
// a meter, where a couple of hundred bytes of punctuation per frame would
// be silly.
//
// *** The layout is append-only. ***
//
// Fields are never reordered, never resized and never repurposed.  A host
// reads the ones it understands and ignores the rest, and treats a frame
// shorter than it expected as "that firmware does not know about the tail"
// rather than as zeroes.  Get that right and every future field is free,
// which is the whole reason this is not self-describing.  The version byte
// is belt and braces - the length is what actually does the work.
//
// Levels go out as -dBFS, one byte per dB.  Full scale is 0 and it counts
// downwards, which suits a 7-bit field: 127dB is more range than the
// converter has, and no sign ever needs sending.
//
static uint8_t level_to_dbfs(float level)
{
	int db;

	// Silence saturates rather than trying to send minus infinity
	if (level <= 0.0f)
		return 127;

	db = -(int)(20.0f * log10f(level));
	if (db < 0)
		db = 0;		// above full scale: still 0dBFS
	if (db > 127)
		db = 127;
	return db;
}

static uint8_t fraction_to_byte(float f)
{
	int v = (int)(f * 127.0f + 0.5f);

	if (v < 0)
		v = 0;
	if (v > 127)
		v = 127;
	return v;
}

//
// The same, where seven bits are not enough.
//
// One step of fraction_to_byte() is 0.787% of whatever is being
// reported, and for the load meter that is coarse enough to be the
// measurement rather than the thing measured: a whole reverb is 26
// steps, so a change worth a couple of percent of one effect is a step
// or two and cannot be told from the quantiser. Sent MIDI's way, as a
// 7-bit MSB and a 7-bit LSB.
//
static uint16_t fraction_to_14bit(float f)
{
	int v = (int)(f * 16383.0f + 0.5f);

	if (v < 0)
		v = 0;
	if (v > 16383)
		v = 16383;
	return v;
}

//
// The expression jack probe's answer.  Bringup only - see exp.h.
//
// Twelve bits will not fit in a SysEx data byte, so each reading goes
// out as seven bits of high and seven of low.  Not a packing scheme,
// just the only shape available.  The readings are followed by what the
// pedal makes of them - see exp_classify() - so that both ends agree
// without the host keeping its own copy of the thresholds, and then by
// what the Exp Jack setting currently says.  Those two disagreeing is
// not an error: the setting is what the pedal runs from and the probe
// only ever proposes, so showing both is how a host offers the change
// rather than making it.
//
#ifdef EXP_TIP_GPIO
struct midi_dest send_exp_tx;
static void sysex_send_exp(void)
{
	struct midi_dest to = send_exp_tx;

	if (!to.to)
		return;

	//
	// The sweep takes 24ms of settling, so it runs as a task in the
	// main loop and this waits for it rather than for the pins.
	// Starting one is ignored while one is already going.
	//
	if (!exp_sweep_ready()) {
		exp_sweep_start();
		return;
	}
	if (midi_tx_busy())
		return;
	send_exp_tx = MIDI_DEST_NONE;

	static const uint8_t hdr[] = { 0xF0, 0x7D, 0x0E };
	static const uint8_t trailer[] = { 0xF7 };

	const uint16_t *reading = exp_sweep_take();
	uint8_t body[3 + 2 * EXP_NR_READINGS];

	body[0] = 3;				// layout version
	for (int i = 0; i < EXP_NR_READINGS; i++) {
		body[1 + 2 * i] = (reading[i] >> 7) & 0x7f;
		body[2 + 2 * i] = reading[i] & 0x7f;
	}
	//
	// What it is, and what it was said to be.  The first prefers the
	// second when the reading is consistent with it - see
	// exp_verify() - so asking a jack that already has a treadle on
	// it answers "still a treadle" rather than giving up because the
	// treadle happens to be parked at its heel stop.
	//
	body[1 + 2 * EXP_NR_READINGS] = exp_verdict(reading);
	body[2 + 2 * EXP_NR_READINGS] = expression.accessory;

	sysex_tx_start_to(to);
	sysex_stream_write(hdr, sizeof(hdr));
	sysex_stream_write(body, sizeof(body));
	sysex_stream_write(trailer, sizeof(trailer));
	midi_tx_commit();
}
#endif

#ifdef NRF54_SWDIO
//
// Whether the pairing window is open, said whenever it changes.
//
// Not an acknowledgement: it is what the pedal is actually doing, so a
// window that timed out says so without being asked.
//
struct midi_dest sysex_send_pairing;

//
// Everyone who has asked about the window since it last closed, which is
// who hears it close.  Not whoever sent the last message: when a bond
// closes it, that is the radio, and when it times out, it is anybody.
// Forgotten once told, so that a Bluetooth peer's number does not outlive
// the peer.
//
static struct midi_dest pairing_watchers;

static void sysex_send_pairing_state(void)
{
	struct midi_dest to = sysex_send_pairing;

	if (!to.to)
		return;
	if (midi_tx_busy())
		return;
	sysex_send_pairing = MIDI_DEST_NONE;

	const uint8_t msg[] = { 0xF0, 0x7D, 0x06, nrf54_pairing, 0xF7 };

	sysex_tx_start_to(to);
	sysex_stream_write(msg, sizeof(msg));
	midi_tx_commit();
}

#ifdef NRF54_SWDIO
//
// The link load test's counts: the pedal's, the radio's as it last
// reported them, and the link's own counters on the pedal's side.  The
// radio is asked again at the same time, so asking twice a moment apart
// gives counts from both sides at the end of a run.
//
struct midi_dest send_linktest_tx;

static void sysex_write_test(const struct linktest *t)
{
	sysex_write_str("{\"sent\":[");
	for (int i = 0; i < LINKTEST_STREAMS; i++) {
		if (i)
			sysex_write_str(",");
		sysex_write_num(t->sent[i]);
	}
	sysex_write_str("],\"got\":");
	sysex_write_num(t->got);
	sysex_write_str(",\"bad\":");
	sysex_write_num(t->bad);
	sysex_write_str(",\"lost\":");
	sysex_write_num(t->lost);
	sysex_write_str(",\"early\":");
	sysex_write_num(t->early);
	sysex_write_str(",\"held\":");
	sysex_write_num(t->held);
	sysex_write_str("}");
}

static void sysex_send_linktest(void)
{
	static const uint8_t header[] = { 0xF0, 0x7D, 0x24 };
	static const uint8_t trailer[] = { 0xF7 };

	if (!send_linktest_tx.to || midi_tx_busy())
		return;

	sysex_tx_start_to(send_linktest_tx);
	sysex_stream_write(header, sizeof(header));
	sysex_write_str("{\"pedal\":");
	sysex_write_test(&nrf54_uart.test);
	sysex_write_str(",\"radio\":");
	sysex_write_test(&nrf54_uart.radio_test);
	sysex_write_str(",\"link\":{\"gaps\":");
	sysex_write_num(nrf54_uart.link.gaps);
	sysex_write_str(",\"refused\":");
	sysex_write_num(nrf54_uart.link.refused);
	sysex_write_str(",\"bad\":");
	sysex_write_num(nrf54_uart.link.rx.bad);
	sysex_write_str(",\"crc\":");
	sysex_write_num(nrf54_uart.link.rx.crc);
	sysex_write_str(",\"tx_bytes\":");
	sysex_write_num(nrf54_uart.tx_bytes);
	sysex_write_str(",\"dropped\":");
	sysex_write_num(nrf54_uart.dropped);
	sysex_write_str(",\"written_off\":");
	sysex_write_num(nrf54_uart.link.written_off);
	// And the stream table, which is what shows a stall
	sysex_write_str(",\"streams\":[");
	for (int i = 0, n = 0; i < LINK_STREAMS; i++) {
		const struct link_stream *st = &nrf54_uart.link.s[i];

		if (!st->used)
			continue;
		sysex_write_str(n++ ? ",[" : "[");
		sysex_write_num(st->kind);
		sysex_write_str(",");
		sysex_write_num(st->peer);
		sysex_write_str(",");
		sysex_write_num(st->tx_next);
		sysex_write_str(",");
		sysex_write_num(st->tx_acked);
		sysex_write_str(",");
		sysex_write_num(st->rx_next);
		sysex_write_str(",");
		sysex_write_num(st->rx_done);
		sysex_write_str("]");
	}
	sysex_write_str("]}}");
	sysex_stream_write(trailer, sizeof(trailer));
	if (sysex_tx_finish("Sent link test counts"))
		send_linktest_tx = MIDI_DEST_NONE;
}
#endif

struct midi_dest send_radio_tx;
//
// How the link to the radio is doing.
//
// 'packets' is MIDI arriving from the radio, which is what a host
// connected over Bluetooth produces; 'dropped' is outgoing bytes that
// found the queue full, which should stay at zero.
//
static void sysex_send_radio(void)
{
	struct midi_dest to = send_radio_tx;

	if (!to.to)
		return;
	if (midi_tx_busy())
		return;
	send_radio_tx = MIDI_DEST_NONE;

	static const uint8_t hdr[] = { 0xF0, 0x7D, 0x0F };
	static const uint8_t trailer[] = { 0xF7 };

	sysex_tx_start_to(to);
	sysex_stream_write(hdr, sizeof(hdr));
	sysex_write_str("{\"baud\":");
	sysex_write_num(NRF54_UART_BAUD);
	sysex_write_str(",\"tx\":");
	sysex_write_num(nrf54_uart.tx_bytes);
	sysex_write_str(",\"rx\":");
	sysex_write_num(nrf54_uart.rx_bytes);
	sysex_write_str(",\"packets\":");
	sysex_write_num(nrf54_uart.packets);
	sysex_write_str(",\"dropped\":");
	sysex_write_num(nrf54_uart.dropped);
	sysex_write_str(",\"listening\":");
	sysex_write_num(nrf54_uart.listening);
	sysex_write_str("}");
	sysex_stream_write(trailer, sizeof(trailer));
	midi_tx_commit();
}
#endif

struct midi_dest send_telemetry_tx;
static void sysex_send_telemetry(void)
{
	struct midi_dest to = send_telemetry_tx;

	if (!to.to)
		return;
	if (midi_tx_busy())
		return;
	send_telemetry_tx = MIDI_DEST_NONE;

	static const uint8_t sysex_telemetry_header[] = { 0xF0, 0x7D, 0x0B };
	static const uint8_t sysex_telemetry_trailer[] = { 0xF7 };

	//
	// The gate multiplier only means anything while the gate is on.
	// Switched off it keeps whatever it last ramped to, which would
	// read as a gate holding the signal down when it is doing nothing
	// of the kind.
	//
	float gate = chain.active ? chain.mult : 1.0f;

	//
	// The load goes out at fourteen bits, as an MSB where the seven-bit
	// version always was and an LSB appended after it.
	//
	// Appending is safe and is how this layout is meant to grow: a
	// reader takes fields while they last and treats a missing one as
	// absent rather than as zero, which is what lets old firmware and
	// new talk to old apps and new in any combination.  WebMIDI's
	// handleTelemetry() is written that way and test-webmidi.js tests
	// both directions.
	//
	// What is *not* pure addition is the byte that was already there:
	// it now carries the top seven bits of a fourteen-bit number rather
	// than a seven-bit rounding of the same quantity, which moves it by
	// at most one step.  A reader that only knows the old layout shows
	// a percentage, so that is at most one percent on a meter - hence
	// the version byte going to 2 rather than this being pretended to
	// be an addition.
	//
	uint16_t load = fraction_to_14bit(meter_load);

	const uint8_t body[] = {
		3,				// layout version
		level_to_dbfs(meter_in),	// input peak, before Trim
		level_to_dbfs(meter_floor),	// the quiet level under it
		level_to_dbfs(meter_out),	// output peak, after Volume
		fraction_to_byte(gate),		// 127 open, 0 fully closed
		load >> 7,			// share of the sample period used
		load & 127,			// ...and the bits under it
#ifdef EXP_TIP_GPIO
		exp_treadle_raw >> 7,		// where the treadle is, raw
		exp_treadle_raw & 127,		// ...and the bits under it
		expression.heel >> 7,		// the travel it has been taught
		expression.heel & 127,
		expression.toe >> 7,
		expression.toe & 127,
#endif
	};

	sysex_tx_start_to(to);
	sysex_stream_write(sysex_telemetry_header, sizeof(sysex_telemetry_header));
	sysex_stream_write(body, sizeof(body));
	sysex_stream_write(sysex_telemetry_trailer, sizeof(sysex_telemetry_trailer));

	//
	// Committed, but not reported.  This is polled, so a frame that
	// does not make it out is replaced by the next one a fifth of a
	// second later, and saying so would be noise about something that
	// fixed itself.
	//
	// Committed all the same.  A transaction that is built and never
	// committed is rewound by whatever starts the next one, so leaving
	// this out does not mean "best effort", it means "never sent".
	//
	midi_tx_commit();
}

struct midi_dest send_schema_tx;
static void sysex_send_schema(void)
{
	if (!send_schema_tx.to)
		return;
	if (midi_tx_busy())
		return;

	static const uint8_t sysex_schema_header[] = { 0xF0, 0x7D, 0x02 };
	static const uint8_t sysex_schema_trailer[] = { 0xF7 };

	//
	// The body is queued where it lies rather than copied.
	//
	// It is a static const string in flash and by far the largest
	// thing the pedal ever says - tens of thousands of bytes, against
	// a state dump's 140 - so copying it into the payload ring would
	// mean sizing that ring for the one reply that least needs it.
	// Three descriptors and four bytes of RAM instead: a generated
	// header, the flash body, a generated trailer.
	//
	sysex_tx_start_to(send_schema_tx);
	sysex_stream_write(sysex_schema_header, sizeof(sysex_schema_header));
	midi_tx_static((const uint8_t *)midi_schema_json, strlen(midi_schema_json));
	sysex_stream_write(sysex_schema_trailer, sizeof(sysex_schema_trailer));
	if (sysex_tx_finish("Sent schema information"))
		send_schema_tx = MIDI_DEST_NONE;
}

struct midi_dest send_status_tx;
static void sysex_send_status(void)
{
	if (!send_status_tx.to)
		return;
	if (midi_tx_busy())
		return;

	static const uint8_t sysex_status_header[] = { 0xF0, 0x7D, 0x09 };
	static const uint8_t sysex_status_trailer[] = { 0xF7 };

	//
	// Always answer, even when there is nothing pending.  A request
	// that gets no reply leaves the host unable to tell "nothing to
	// report" from "the reply went missing", which for a diagnostic is
	// the wrong way round: silence is exactly what you cannot trust
	// when you are already asking why something is quiet.
	//
	// 'status' stays NULL in that case rather than becoming the empty
	// string, because the two are not interchangeable below: putting an
	// empty string back into the mailbox would be a message as far as
	// report_info() is concerned, and would block every later one.
	//
	const char *status = get_status();

	sysex_tx_start_to(send_status_tx);
	sysex_stream_write(sysex_status_header, sizeof(sysex_status_header));
	if (status)
		sysex_stream_write((const uint8_t *)status, strlen(status));
	sysex_stream_write(sysex_status_trailer, sizeof(sysex_status_trailer));

	//
	// Put it back if it didn't get out.  get_status() took it before
	// the transmit was attempted, and usb_midi_write() is best-effort
	// with a 20ms deadline, so a congested link would otherwise destroy
	// the one message that mattered - at what is a plausible moment for
	// whatever is being diagnosed to be happening.
	//
	// report_info() rather than report_status() is the whole of it: it
	// restores the message only if nothing newer has arrived, and
	// something newer is by definition more current.  Not a
	// self-reference either - this puts the original back rather than
	// reporting a new message about the failure, which is why
	// sysex_tx_finish() is still the wrong thing to call here.  That
	// would report a failed status report as a status report, and start
	// a conversation with itself.
	//
	// The window this guards is much narrower now that a full transmit
	// fifo is a wait rather than a loss - what is left is the queue
	// itself being full, which is rarer and is still not a reason to
	// destroy the one message somebody was asking for.
	//
	if (midi_tx_commit())
		send_status_tx = MIDI_DEST_NONE;
	else if (status)
		report_info(status);
}

//
// Several of one effect's pots in one message.
//
//	F0 7D 03 <eff> [<pot> <val>] ... F7
//
// Seven bytes to carry three is four bytes of framing in every message,
// and the state dump used to send one per pot.  Grouping by effect rather
// than allowing arbitrary (effect, pot, value) triples is what makes each
// extra pot two bytes instead of three, and it costs nothing to arrange
// because the dump already walks effects with their pots inside.
//
// A message with one pair is byte for byte what was always sent, so this
// is a superset rather than a change of format, and sysex_echo_pot()
// below sends exactly that one-pair form when the pedal moves a pot by
// itself.
//
// Big enough for every pot an effect can have at once: the mix, POT_LAST
// real ones, and the three steering pots.
//
struct pot_batch {
	uint8_t buf[4 + 2 * (1 + POT_LAST + 3) + 1];
	unsigned int len;
};

static void pot_batch_start(struct pot_batch *b, int eff)
{
	b->buf[0] = 0xF0;
	b->buf[1] = 0x7D;
	b->buf[2] = 0x03;
	b->buf[3] = eff;
	b->len = 4;
}

static void pot_batch_add(struct pot_batch *b, int pot, int value)
{
	// Same "should never happen" as the single-pot version.
	if (value < 0 || value > 120) value = 0;

	if (b->len + 2 > sizeof(b->buf) - 1)
		return;
	b->buf[b->len++] = pot;
	b->buf[b->len++] = value;
}

static void pot_batch_send(struct pot_batch *b)
{
	// Nothing added is nothing to say, not an empty message.
	if (b->len <= 4)
		return;
	b->buf[b->len++] = 0xF7;
	sysex_stream_write(b->buf, b->len);
}

//
// Tell the host about a pot the pedal moved by itself.
//
// A footswitch or an encoder changing a value is the one case where the
// host's picture goes stale without the host having done anything, so it
// gets told.  Through the queue rather than straight at USB, for two
// reasons that are both about it being three packets:
//
// Truncation.  A blocking write gives up after 20ms, so the old path
// could put out 'F0 7D 03' and then not the rest - and Web MIDI only
// delivers terminated messages, so a half-message is not merely late, it
// is a parser left waiting.  Committing the whole thing or none of it
// is exactly what the queue does.
//
// And waiting.  Turning a knob against a host that has stopped reading
// used to cost 60ms of stalled main loop per step, which is audio
// somebody is listening to being spent on a message nobody is receiving.
//
// Dropped when the queue is busy, and that is fine here: the next state
// dump carries the same value, so the app's picture repairs itself
// without this having to be reliable.
//
// Which is exactly why several pots at once has to be one call and not
// several.  'Busy' means the queue is not empty, so the first of two
// back-to-back echoes fills it and the second is dropped every single
// time - not occasionally, every time, and always the same one.  A
// treadle's calibration was sending Heel and then Toe, and Toe never
// arrived once.  One message carrying both is what the batch format was
// added for; the singular below is the one-pair case of it.
//
// The plural says whether it went, so a caller that can try again knows
// whether it needs to.
//
static bool sysex_echo_pots(int eff, const uint8_t *pairs, int n)
{
	struct pot_batch batch;

	if (midi_tx_busy())
		return false;

	pot_batch_start(&batch, eff);
	for (int i = 0; i < n; i++)
		pot_batch_add(&batch, pairs[2 * i], pairs[2 * i + 1]);

	// For every editor, not only whoever moved it
	sysex_tx_start_to(MIDI_DEST_ALL);
	pot_batch_send(&batch);

	// Which is what committed, not what was queued: a transaction that
	// ran out of room part way through rewinds, and a caller told 'yes'
	// about that would never send it again.
	return midi_tx_commit();
}

static void sysex_echo_pot(int eff, int pot, int val)
{
	const uint8_t pair[2] = { pot, val };

	sysex_echo_pots(eff, pair, 1);
}

//
// Say what the physical controls are bound to.
//
// The whole table in one message, because the app wants all of it or
// none of it - a rule only means anything alongside the others that
// share its gesture.  Sent as part of the
// state dump, and again whenever the pedal changes a binding by itself
// - which it does when a gesture is bound to ACT_NEXT_POT, the one
// action whose effect is to move another binding.
//
// The message itself, with no transmit session of its own, so that it
// can go inside somebody else's.
//
// One level of the rule table.
//
// Four of them, and only two can be written: a scene's rules, the
// pedal-wide ones, what those resolve to, and what is compiled in.  The
// last two are answers rather than settings.
//
// RULES_EFFECTIVE is the one worth having on the wire.  It is what a
// gesture actually does, which is not something the app can work out
// from the other two without implementing the shadowing a second time -
// and two implementations of one rule is how the pot defaults and the
// graph Q both went wrong before.
//
// RULES_DEFAULT closes the other gap.  The compiled-in table has never
// been readable at all, so an app showing "this control is unbound"
// could not say what would happen instead.
//
enum rule_level {
	RULES_SCENE,
	RULES_GLOBAL,
	RULES_EFFECTIVE,
	RULES_DEFAULT,
	NR_RULE_LEVELS,
};

static const struct rule *rule_level(unsigned int level, unsigned int *count)
{
	switch (level) {
	case RULES_SCENE:
		*count = nr_scene_rules;
		return scene_rules;
	case RULES_GLOBAL:
		*count = nr_global_rules;
		return global_rules;
	case RULES_EFFECTIVE:
		*count = nr_rules;
		return rules;
	default:
		*count = ARRAY_SIZE(default_rules);
		return default_rules;
	}
}

static void sysex_write_bindings(unsigned int level)
{
	const uint8_t header[] = { 0xF0, 0x7D, 0x0d, level };
	static const uint8_t trailer[] = { 0xF7 };
	uint8_t table[EFFECTIVE_RULES * 6];
	unsigned int count;
	const struct rule *src = rule_level(level, &count);

	for (unsigned int i = 0; i < count; i++) {
		table[i*6 + 0] = src[i].control;
		table[i*6 + 1] = src[i].action;
		table[i*6 + 2] = src[i].effect;
		table[i*6 + 3] = src[i].pot;
		table[i*6 + 4] = src[i].val[0];
		table[i*6 + 5] = src[i].val[1];
	}

	sysex_stream_write(header, sizeof(header));
	sysex_stream_write(table, count * 6);
	sysex_stream_write(trailer, sizeof(trailer));
}

static void sysex_send_bindings(unsigned int level)
{
	sysex_tx_start();
	sysex_write_bindings(level);
	sysex_tx_finish("Sent control bindings");
}

struct midi_dest state_dump_tx;
static void sysex_send_state_dump(void)
{
	if (!state_dump_tx.to)
		return;
	if (midi_tx_busy())
		return;

	//
	// Send the global enable state.  A plain CC rather than SysEx, and
	// if it will not go then there is no point starting on the rest.
	//
	// It used to be a probe for "is anybody reading", waiting up to
	// 20ms to find out.  It no longer has to be: a full transmit fifo
	// now means the endpoint is busy this instant, which is a reason to
	// come back next time round the main loop and not a reason to wait.
	// The request flag stays set, so nothing is lost by leaving.
	//
	report_info("Sending global-enable state");
	uint8_t cc_packet[4] = { 0x0B, 0xB0, MIDI_CC_GLOBAL_ENABLE, disable_all ? 0 : 127 };
	if (!usb_midi_write_or_drop(cc_packet))
		return;
	radio_midi_write(cc_packet);

	//
	// One give-up flag for the whole dump rather than one per
	// message.  It is a couple of hundred messages, so retrying each
	// in turn against a host that has stopped reading would block
	// core 0 for the timeout times the message count - seconds.
	// Once it is set every write below quietly does nothing.
	//
	sysex_tx_start_to(state_dump_tx);

	//
	// Then the effect states - for the effects that have one.
	//
	// An effect that is not in the chain is not running, and
	// routing_end() has reset it to the schema defaults the app
	// already knows, so sending its pots is telling the app what it
	// told us.  It was most of the dump: 1050 of 1099 pot bytes on a
	// board with one effect routed, which is 95% of the traffic
	// describing effects that do nothing.
	//
	// This is only correct while "unrouted means schema defaults" is
	// the model.  If the pedal ever keeps values for effects it is
	// not running, those values become real state and belong here
	// again.
	//
	report_info("Sending effect pot state");
	for (int i = 0; i < ARRAY_SIZE(effects); i++) {
		struct effect *e = effects[i];
		const struct pot_descr *desc = e->pots;
		unsigned char *pot_values = effect_pots(e);
		struct pot_batch batch;

		if (!effect_present(i))
			continue;
		if (!effect_always_runs(i) && !effect_is_routed(e))
			continue;

		pot_batch_start(&batch, i);

		// We send the mix as "pot 0", and then pots numbered from 1
		pot_batch_add(&batch, POT_MIX, FLOAT_TO_POT(e->mix_pot));
		for (int pot = 0; pot < POT_LAST; pot++) {
			if (!desc[pot].label)
				break;
			pot_batch_add(&batch, pot+1, pot_values[pot]);
		}

		//
		// ...and where it sits across the channels, for the
		// effects that have somewhere to sit.  Sent as pots so
		// that an app which does not know these numbers yet
		// ignores three more of what it was already reading.
		//
		if (!e->no_mix) {
			pot_batch_add(&batch, POT_CH_IN, CH_IN(e->channels));
			pot_batch_add(&batch, POT_CH_OUT, CH_OUT(e->channels));
			pot_batch_add(&batch, POT_MERGE, FLOAT_TO_POT(e->merge));
		}

		pot_batch_send(&batch);
	}

	//
	// What the pedal's own controls are bound to - every level of
	// it.  The dump is what the app builds its whole picture from,
	// and "what this control does" and "why" are different
	// questions that it now has to be able to answer separately.
	//
	report_info("Sending control bindings");
	for (unsigned int level = 0; level < NR_RULE_LEVELS; level++)
		sysex_write_bindings(level);

	//
	// And finally the routing order, which stays last because the app
	// takes it as the end of the dump.
	//
	report_info("Sending routing information");
	static const uint8_t sysex_routing_header[] = { 0xF0, 0x7D, 0x08 };
	static const uint8_t sysex_routing_trailer[] = { 0xF7 };

	sysex_stream_write(sysex_routing_header, sizeof(sysex_routing_header));
	sysex_stream_write(effect_chain, routed_effect_count);
	sysex_stream_write(sysex_routing_trailer, sizeof(sysex_routing_trailer));

	if (sysex_tx_finish("Sent state dump"))
		state_dump_tx = MIDI_DEST_NONE;
}

//
// Big enough for the largest thing that arrives, which is the rule
// table: six bytes each and a command byte in front.
//
// Named rather than taken with sizeof: handle_sysex_payload() has a
// parameter of the same name, so sizeof inside it is the size of a
// pointer.
//
//
// The longest inbound SysEx payload the pedal will assemble.
//
// A rule table sized it - one command byte and six per rule - and the
// radio's diagnostics now arrive this way too, so it has a floor with room
// for one of those.  A longer message is dropped whole, and silently as
// far as the sender can tell.
//
#define SYSEX_BUF_MAX	((1 + MAX_RULES * 6) > 192 ? (1 + MAX_RULES * 6) : 192)

//
// SysEx being put back together, one message per source: USB, the jacks,
// the radio's own answers, and each Bluetooth peer.  The radio link
// interleaves its streams a packet at a time, so a message from one source
// can arrive while another's is half assembled, and one buffer between
// them would splice the two.
//
#define SYSEX_SOURCES	6

static struct sysex_in {
	struct midi_dest from;
	uint8_t buf[SYSEX_BUF_MAX];
	int len;
	bool in;			// between F0 and F7
	bool over;			// longer than buf: drop it
	uint32_t used;
} sysex_in[SYSEX_SOURCES];
static uint32_t sysex_clock;

//
// The slot for a source: its own, else one with no message half built,
// else the one used longest ago.
//
static struct sysex_in *sysex_slot(struct midi_dest from)
{
	struct sysex_in *pick = NULL;

	for (int i = 0; i < SYSEX_SOURCES; i++) {
		struct sysex_in *in = &sysex_in[i];

		if (in->used && in->from.to == from.to &&
		    in->from.peer == from.peer) {
			in->used = ++sysex_clock;
			return in;
		}
	}
	for (int i = 0; i < SYSEX_SOURCES; i++) {
		struct sysex_in *in = &sysex_in[i];

		if (!pick || (!in->in && pick->in) ||
		    (in->in == pick->in &&
		     (int32_t)(in->used - pick->used) < 0))
			pick = in;
	}
	pick->from = from;
	pick->len = 0;
	pick->in = pick->over = false;
	pick->used = ++sysex_clock;
	return pick;
}


// Set by the dispatch below, taken by hum_task()
static unsigned int hum_learn_request;	// -dBFS, or 128 for a clear (00 sent)
static bool hum_report_request;

static void handle_sysex_payload(uint8_t *sysex_buf, size_t sysex_len)
{
	uint8_t cmd = sysex_buf[0];
	if (cmd == 0x01) { // Schema Request
		midi_dest_add(&send_schema_tx, midi_from);
	} else if (cmd == 0x03 && sysex_len >= 4) { // Set Parameter
		//
		// One effect, and then as many (pot, value) pairs as the
		// message carries - the same shape the state dump sends.
		//
		// An odd number of bytes after the effect id is a message
		// somebody built wrong, and the last half-pair is dropped
		// rather than guessed at.  A message too long to fit was
		// already refused whole by the accumulator, so a short
		// list here is a short list somebody meant to send.
		//
		uint8_t eff_id = sysex_buf[1];
		struct effect *e = NULL;
		if (eff_id < ARRAY_SIZE(effects)) {
			e = effects[eff_id];
		}
		for (size_t i = 2; e && i + 1 < sysex_len; i += 2) {
			uint8_t pot_idx = sysex_buf[i];
			uint8_t val = sysex_buf[i + 1];

			if (pot_idx == POT_MIX)
				set_effect_mix(e, val > 120 ? 120 : val);
			else if (pot_idx <= POT_LAST) {
				//
				// Clamped against the same max_pot_val() the
				// load path validates with, because a value
				// this stores is a value that has to load
				// again.
				//
				// scene_load_effect() refuses a whole scene
				// that has a pot past its maximum, and
				// max_pot_val() is 120 for an ordinary pot,
				// the last index for an enum, and zero for a
				// slot the effect does not use.  So a MIDI
				// value of 121..127, or anything non-zero
				// written to a pot that is not there, used to
				// be stored happily and then make that scene
				// fail to load for good - silently, because
				// the checksum is computed at save time from
				// whatever was stored, so the scene looks
				// intact and simply comes back as defaults.
				//
				// Clamped rather than ignored: a controller
				// sending a full 0..127 sweep is a real thing
				// to be, and "the pot went to its maximum" is
				// a better answer to that than "the last part
				// of the sweep did nothing".
				//
				int max = max_pot_val(e, pot_idx - 1);
				set_effect_pot(e, pot_idx - 1,
					       val > max ? max : val);
			}
			//
			// Steering is meaningless for an effect that
			// is not stepped through do_effect_step() at
			// all, which is what no_mix marks.
			//
			else if (pot_idx <= POT_MAX && !e->no_mix)
				set_effect_steering(e, pot_idx, val);
		}
	} else if (cmd == 0x04 && sysex_len >= 2) { // Save Scene
		uint8_t scene_id = sysex_buf[1];

		//
		// The settings go too, even though they are no longer
		// part of the scene.  They used to be saved by being in
		// its last slot, and "Save" meaning "keep what I have
		// set up" is what anybody pressing it expects - the
		// change is where they are kept, not when.
		//
		save_scene(scene_id);
		save_globals();

	} else if (cmd == 0x09) { // Diagnostic Request

		midi_dest_add(&send_status_tx, midi_from);

	} else if (cmd == 0x0a) { // Identity Request

		midi_dest_add(&send_identity_tx, midi_from);

	} else if (cmd == 0x0b) { // Telemetry Request

		midi_dest_add(&send_telemetry_tx, midi_from);

#ifdef NRF54_SWDIO
	} else if (cmd == 0x0f) { // Radio link - bringup only

		midi_dest_add(&send_radio_tx, midi_from);

	} else if (cmd == 0x06) {
		//
		// The pairing window: 01 opens it, 00 closes it, and the
		// command on its own asks without changing anything.
		//
		// No duration is taken.  How long it stays open is the
		// pedal's business, because the pedal is the thing that has
		// to show it and time it.
		//
		// Either transport may change it, and over the air that is
		// not the hole it looks like: the characteristic requires a
		// bond made with Secure Connections, so anything that can
		// send this was let in through a window somebody already had
		// to open.  A device trusted enough to edit every parameter
		// is trusted enough to introduce the next one.
		//
		if (sysex_len >= 2) {
			bool want = sysex_buf[1] != 0;

			nrf54_pairing = want;
			nrf54_pairing_until =
				to_ms_since_boot(get_absolute_time()) +
				NRF54_PAIRING_MS;
			nrf54_pairing_tell(want);
		}
		midi_dest_add(&pairing_watchers, midi_from);
		midi_dest_add(&sysex_send_pairing, midi_from);

	} else if (cmd == 0x07 && !sysex_from_radio) {
		//
		// Forget every paired device.  The radio holds the keys, so
		// this only passes the word along; the radio's own reply
		// carries the count afterwards.
		//
		// Over the cable only, and for two reasons that happen to
		// agree.  It is destructive over the air in the plainest
		// way - it drops the key of the link carrying the request,
		// and without a cable there is no way back - and a stranger
		// should not be able to wipe what a pedal is paired to.
		//
		nrf54_forget_bonds();

	} else if (cmd == 0x18 && sysex_from_radio) {
		//
		// The radio bonded with somebody.  The window has done what
		// it was opened for, so close it rather than leave it open
		// for whoever is next.
		//
		nrf54_pairing = false;
		nrf54_pairing_tell(false);
		midi_dest_add(&sysex_send_pairing, pairing_watchers);
		pairing_watchers = MIDI_DEST_NONE;

	} else if (cmd == 0x16 && sysex_from_radio && sysex_len >= 2) {
		//
		// The one thing in the radio's range that is addressed to
		// the pedal: whether anything past the radio has asked to
		// be sent MIDI.  Consumed rather than forwarded - the app
		// is the subscriber and does not need telling what it just
		// did.
		//
		nrf54_uart.listening = sysex_buf[1] != 0;

	} else if (cmd == 0x19 && sysex_from_radio) {
		//
		// Forgetting a key is the radio's own command, and this is
		// the one of those the pedal will not pass on from over the
		// air - the same rule 0x07 above follows, which would
		// otherwise be sidestepped by addressing the radio
		// directly.
		//

	} else if (cmd == 0x1d && sysex_from_radio) {
		//
		// MIDI for the device the radio is bound to is the pedal's to
		// send.  Passed on from over the air, it would let anything
		// that can reach the radio drive that device.
		//

	} else if (cmd == 0x1f && sysex_from_radio) {
		//
		// The radio's name is the pedal's to give.  Passed on from
		// over the air, it would let any client rename the radio.
		//

	} else if (cmd >= 0x10 && cmd <= 0x1f) {
		//
		// Addressed to the radio rather than to the pedal: a
		// scan, what a scan found, and what it is paired with.  The
		// pedal is the wire between the app and the radio and reads
		// almost none of it.
		//
		// Which way it goes is the only decision here, and it is
		// the one thing the message itself does not say.
		//
		uint8_t msg[3 + SYSEX_BUF_MAX];
		size_t n = 0;

		msg[n++] = 0xF0;
		msg[n++] = 0x7D;
		for (size_t i = 0; i < sysex_len && n < sizeof(msg) - 1; i++)
			msg[n++] = sysex_buf[i];
		msg[n++] = 0xF7;

		if (sysex_from_radio) {
			//
			// midi_tx_bytes() only adds to a pending payload;
			// it takes a transaction around it to become a
			// message anyone will send.
			//
			sysex_tx_start_to(MIDI_DEST_USB);
			sysex_stream_write(msg, n);
			midi_tx_commit();
		} else {
			nrf54_uart_write(msg, n);
		}
#endif

#ifdef EXP_TIP_GPIO
	} else if (cmd == 0x0e) { // Expression jack probe - bringup only

		midi_dest_add(&send_exp_tx, midi_from);
#endif

	} else if (cmd == 0x21) { // Learn the hum cuts, or clear them

		hum_learn_request = sysex_len < 2 ? 95 :
				    sysex_buf[1] ? sysex_buf[1] : 128;

	} else if (cmd == 0x22) { // Report the hum cuts

		hum_report_request = true;

#ifdef NRF54_SWDIO
	} else if (cmd == 0x23 && sysex_len >= 9) { // Start a link load test
		//
		// The settings, seven bits a byte, in the order linktest.h
		// packs them: test streams, packets on each (two bytes), the
		// stuck stream or 7F, how long it stays stuck in ms (two
		// bytes), and debug lines for the radio to print (two bytes).
		//
		struct linktest_cfg cfg;

		sysex_buf[0] = LINKTEST_START;
		if (linktest_cfg_unpack(sysex_buf, sysex_len, &cfg))
			nrf54_test_start(&cfg);

	} else if (cmd == 0x24) { // The link load test's counts

		nrf54_test_ask();
		midi_dest_add(&send_linktest_tx, midi_from);
#endif

	} else if (cmd == 0x05) { // State Dump Request

		midi_dest_add(&state_dump_tx, midi_from);

	} else if (cmd == 0x0c && sysex_len >= 2) { // Write one rule level
		unsigned int level = sysex_buf[1];

		//
		// Echo it back, always, and not as an acknowledgement:
		// rules that do not check out are dropped, so what came
		// back is the answer to "what did you keep".  That is
		// also the case where the two ends disagree, which is
		// exactly when the app needs telling.
		//
		// A write to a level that is computed rather than kept
		// is refused rather than ignored - it still answers, with
		// the level unchanged, so the app sees that nothing
		// happened instead of assuming it worked.
		//
		if (level == RULES_SCENE || level == RULES_GLOBAL) {
			struct rule *dst = level == RULES_SCENE ?
				scene_rules : global_rules;
			unsigned int *n = level == RULES_SCENE ?
				&nr_scene_rules : &nr_global_rules;

			set_rules(dst, n, sysex_buf + 2, (sysex_len - 2) / 6);
		}
		if (level < NR_RULE_LEVELS)
			sysex_send_bindings(level);

	} else if (cmd == 0x0d && sysex_len >= 2) { // Read one rule level
		//
		// Separate from the write, because "set the table to
		// nothing" and "tell me the table" would otherwise be
		// the same message - and clearing a level by asking to
		// look at it is a memorable way to lose work.
		//
		// Same command number as the reply, which is how 0x0a
		// and 0x0b already work.
		//
		if (sysex_buf[1] < NR_RULE_LEVELS)
			sysex_send_bindings(sysex_buf[1]);

	} else if (cmd == 0x08) { // Set Routing Order
		routing_bitmap_t routable = routing_start();

		for (int i = 1; i < sysex_len; i++)
			routing_add(&routable, sysex_buf[i]);

		routing_end(routable);
	}
}


static bool handle_midi_packet_as(const uint8_t packet[4]);

//
// A message from someone in particular, who any reply to it is for.
//
bool handle_midi_packet_from(const uint8_t packet[4],
			     struct midi_dest from)
{
	bool handled;

	midi_from = from;
	handled = handle_midi_packet_as(packet);
	midi_from = MIDI_DEST_ALL;
	return handled;
}

// From USB, which is where usb-device.c finds this
bool handle_midi_packet(const uint8_t packet[4])
{
	return handle_midi_packet_from(packet, MIDI_DEST_USB);
}

static bool handle_midi_packet_as(const uint8_t packet[4])
{
	uint8_t code = packet[0] & 0x0F;

	// Handle SysEx parsing across packets
	if (code == 0x04 || code == 0x05 || code == 0x06 || code == 0x07) {
		struct sysex_in *in = sysex_slot(midi_from);

		for (int i = 1; i <= 3; i++) {
			uint8_t b = packet[i];
			if (b == 0xF0) {
				in->in = true;
				in->len = 0;
				in->over = false;
			} else if (b == 0xF7 && in->in) {
				in->in = false;
				//
				// A message that did not fit is dropped whole
				// rather than acted on short.
				//
				// The bytes past the end used to be discarded
				// while the rest was handled anyway, which is
				// only harmless while every message is a fixed
				// size - a truncated one then fails its length
				// check and goes nowhere.  It stops being
				// harmless the moment a message carries a list,
				// because a short list is a valid shorter list:
				// half the pots in a batch would be set and the
				// other half silently ignored.
				//
				if (in->over)
					report_info("MIDI message too long, dropped");
				else
					handle_sysex_payload(in->buf, in->len);
			} else if (in->in) {
				if (in->len == 0 && b == 0x7D) {
					// Consume header 7D
				} else if (in->len < SYSEX_BUF_MAX) {
					in->buf[in->len++] = b;
				} else {
					in->over = true;
				}
			}
			if (code == 0x05 && i == 1) break;
			if (code == 0x06 && i == 2) break;
		}
		return true;
	}

	uint8_t status = packet[1];
	uint8_t data1 = packet[2];
	uint8_t data2 = packet[3];

	if (settings.midi_channel != 0) {
		if ((status & 0x0F) != (settings.midi_channel - 1))
			return false;
	}

	bool handled = false;

	if ((status & 0xF0) == 0xB0) {
		handled = true;
		if (data1 == 20) { // Global Bypass
			if (data2 == 68) {
				analyzer_set_mode(ANALYZE_TUNER);
			} else if (data2 == 69) {
				if (analyzer_mode == ANALYZE_TUNER)
					analyzer_set_mode(ANALYZE_OFF);
			} else if (data2 == 126) {
				reset_usb_boot(0, 0);
			} else {
				disable_all = (data2 == 0) ? EFF_ENABLE_STEPS : 0;
			}
		} else if (data1 == 7) { // Main volume
			//
			// The far end of the signal chain.  CC data is
			// 0-127 and a pot is 0-120, so scale rather than
			// clamp - a controller at full should mean unity,
			// not seven counts short of it.
			//
			set_effect_pot(effects[0], CHAIN_VOLUME,
				       data2 * 120 / 127);
		}
	} else if ((status & 0xF0) == 0xC0) {
		handled = true;
		// Program Change -> Load Scene
		if (data1 < MAX_SCENES && (populated_scenes() & (1u << data1))) {
			load_scene(data1);
			state_dump_tx = true;
		}
	}

	return handled;
}

#endif
