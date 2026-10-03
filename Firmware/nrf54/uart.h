#ifndef NRF54_UART_H
#define NRF54_UART_H

#ifdef NRF54_SWDIO

#include "nRF54/app/src/link.h"

//
// The link to the radio, as packets (nRF54/app/src/link.h): MIDI to and
// from Bluetooth, commands for the radio and its answers, and its debug
// text, each on streams of its own.  What happens to the MIDI after the
// radio - the Bluetooth packets, their timestamps, the attribute they are
// written to - is the radio's business.
//
// Flow control is the board's normal state rather than an option.  All
// four wires exist, and the nRF's CTS is active low with a pull-up on it,
// so a floating CTS reads as "not clear to send" and the radio never
// transmits at all.
//
// Not to be confused with midi/uart.h, which is MIDI over the TRS jacks
// on the boards that have them.  This board has none, which is why uart1
// is free for this.
//

//
// Both directions are DMA, which is what removes the deadline.  The
// main loop shares a pass with tud_task(), dap_poll() and building a
// SysEx reply, so it cannot promise to empty a 32-byte FIFO inside the
// 320 us it takes to fill at 1 Mbit.  The hardware writes into a ring
// instead and the loop reads whatever has appeared, so arriving late
// costs latency and not data.
//
// The receive ring is aligned to its own size because the DMA wraps it
// with an address mask.
//
#define NRF54_TX_RING_SHIFT	10
#define NRF54_TX_RING_SIZE	(1 << NRF54_TX_RING_SHIFT)
#define NRF54_TX_RING_MASK	(NRF54_TX_RING_SIZE - 1)

#define NRF54_RX_RING_SHIFT	12
#define NRF54_RX_RING_SIZE	(1 << NRF54_RX_RING_SHIFT)
#define NRF54_RX_RING_MASK	(NRF54_RX_RING_SIZE - 1)

//
// Commands waiting for the radio, whole, each behind a length byte.  None
// is longer than a SysEx message the pedal accepts, so a byte is enough.
//
#define NRF54_CTL_RING_SHIFT	9
#define NRF54_CTL_RING_SIZE	(1 << NRF54_CTL_RING_SHIFT)
#define NRF54_CTL_RING_MASK	(NRF54_CTL_RING_SIZE - 1)

// How often acknowledgements are said again (nRF54/app/src/link.h)
#define NRF54_ACK_AGAIN_MS	100

static struct {
	uint8_t tx[NRF54_TX_RING_SIZE];
	uint16_t tx_head, tx_tail;
	uint16_t tx_inflight;		// handed to the DMA, not yet gone

	uint8_t rx[NRF54_RX_RING_SIZE]
		__attribute__((aligned(NRF54_RX_RING_SIZE)));
	uint16_t rx_tail;

	int dma_tx, dma_rx;

	//
	// The link is packets (nRF54/app/src/link.h), and each kind of
	// traffic is its own stream with its own window.  MIDI on its way
	// out collects into 'midi' until a packet is full or the main loop
	// comes round; commands wait whole in 'ctl'.  What comes in is MIDI
	// and the radio's answers, each through a parser of its own so that
	// one cannot land in the middle of the other.
	//
	struct link link;
	uint8_t midi[LINK_PAYLOAD_MAX];
	uint16_t midi_len;
	uint8_t ctl[NRF54_CTL_RING_SIZE];
	uint16_t ctl_head, ctl_tail, ctl_sent;
	struct midi_parser parser, ctl_parser;
	uint32_t acked_at;		// when acks were last said again

	uint32_t tx_bytes, rx_bytes;
	uint32_t packets, dropped;
	bool up;

	//
	// Whether anything on the far side of the radio has asked to be
	// sent MIDI.
	//
	// False until the radio says otherwise, which it does whenever the
	// answer changes.  A Bluetooth client that has connected without
	// subscribing is the ordinary case rather than a rare one, and
	// sending a 36 kB schema to one costs the link the whole transfer
	// and then loses it a packet at a time at the far end.
	//
	bool listening;
} nrf54_uart;

//
// Where the receive DMA has got to.
//
// It never stops, so there is no completion to wait for and nothing to
// restart: the write address is the producer's index and wraps with the
// ring, the same way i2s_dma_rx_ptr() reads the audio ring.
//
static inline uint16_t nrf54_rx_head(void)
{
	uintptr_t at = dma_hw->ch[nrf54_uart.dma_rx].write_addr;

	// The ring is read after this, not before
	__dmb();
	return (uint16_t)(at - (uintptr_t)nrf54_uart.rx) & NRF54_RX_RING_MASK;
}

// Bytes in the transmit ring
static uint16_t nrf54_tx_used(void)
{
	return (nrf54_uart.tx_head - nrf54_uart.tx_tail) & NRF54_TX_RING_MASK;
}

//
// Bytes into the transmit ring, all of them or none.
//
static bool nrf54_uart_put(const uint8_t *buf, size_t len)
{
	uint16_t used = nrf54_tx_used();

	if (len > NRF54_TX_RING_MASK - used)
		return false;
	for (size_t i = 0; i < len; i++) {
		nrf54_uart.tx[nrf54_uart.tx_head] = buf[i];
		nrf54_uart.tx_head = (nrf54_uart.tx_head + 1) &
				     NRF54_TX_RING_MASK;
	}
	return true;
}

static bool nrf54_uart_room(void)
{
	uint16_t used = nrf54_tx_used();

	return NRF54_TX_RING_MASK - used >= LINK_WIRE_MAX;
}

//
// Whatever can go out now: acknowledgements first, since they cost the
// radio nothing and free its windows, then a packet from each stream that
// has something and room in its window, in turn.
//
static void nrf54_link_out(void)
{
	uint8_t wire[LINK_WIRE_MAX];
	bool moved;
	size_t n;

	while (nrf54_uart_room() && (n = link_pack_ack(&nrf54_uart.link, wire)))
		nrf54_uart_put(wire, n);

	if (!nrf54_uart.link.up)
		return;

	do {
		moved = false;

		if (nrf54_uart.midi_len && nrf54_uart_room() &&
		    link_can_send(&nrf54_uart.link, LINK_MIDI, LINK_ALL)) {
			uint8_t first = nrf54_uart.midi[0];

			//
			// MIDI says where its own messages end; what the
			// receiver needs after a lost packet is where the next
			// one starts, which is a status byte that is not F7.
			//
			n = link_pack(&nrf54_uart.link, LINK_MIDI, LINK_ALL,
				      (first & 0x80) && first != 0xF7 ?
				      LINK_FIRST : 0,
				      nrf54_uart.midi, nrf54_uart.midi_len, wire);
			nrf54_uart_put(wire, n);
			nrf54_uart.midi_len = 0;
			moved = true;
		}


		if (nrf54_uart.ctl_tail != nrf54_uart.ctl_head &&
		    nrf54_uart_room() &&
		    link_can_send(&nrf54_uart.link, LINK_CONTROL, LINK_ALL)) {
			uint8_t chunk[LINK_PAYLOAD_MAX];
			uint16_t at = nrf54_uart.ctl_tail;
			uint8_t len = nrf54_uart.ctl[at & NRF54_CTL_RING_MASK];
			uint16_t left = len - nrf54_uart.ctl_sent;
			uint16_t take = left < LINK_PAYLOAD_MAX ? left
							       : LINK_PAYLOAD_MAX;
			uint8_t flags = 0;

			for (uint16_t i = 0; i < take; i++)
				chunk[i] = nrf54_uart.ctl[(at + 1 +
					nrf54_uart.ctl_sent + i) &
					NRF54_CTL_RING_MASK];
			if (!nrf54_uart.ctl_sent)
				flags |= LINK_FIRST;
			nrf54_uart.ctl_sent += take;
			if (nrf54_uart.ctl_sent == len) {
				flags |= LINK_LAST;
				nrf54_uart.ctl_tail = at + 1 + len;
				nrf54_uart.ctl_sent = 0;
			}
			n = link_pack(&nrf54_uart.link, LINK_CONTROL, LINK_ALL,
				      flags, chunk, take, wire);
			nrf54_uart_put(wire, n);
			moved = true;
		}
	} while (moved);
}

//
// Is there room for another byte on its way to the radio?
//
// Asked by midi_tx_to_radio() before it takes a byte off a message, so
// that a full packet pauses this consumer rather than losing the middle of
// a SysEx.  The packet waits for its stream's window, and the window waits
// for the radio to have put the packet before it on the air, so a
// Bluetooth client that cannot keep up stops this consumer, and only this
// one: USB carries on, which is what the cursors are for.
//
// Three bytes, because a channel message is written whole after one ask.
//
#define NRF54_MIDI_ROOM	3

static bool nrf54_uart_ready(void)
{
	if (nrf54_uart.midi_len > LINK_PAYLOAD_MAX - NRF54_MIDI_ROOM)
		nrf54_link_out();
	return nrf54_uart.link.up &&
	       nrf54_uart.midi_len <= LINK_PAYLOAD_MAX - NRF54_MIDI_ROOM;
}

// Defined below, and declared here because the senders above it call it.
static void nrf54_uart_write(const uint8_t *buf, size_t len);

//
// One outgoing MIDI byte, on its way to the radio.
//
// Called from midi_tx_to_radio(), which walks the send queue with a
// cursor of its own and asks nrf54_uart_ready() first.  Declared in
// midi/tx.h because that file comes first; see the note there.
//
static void nrf54_uart_thru(uint8_t byte)
{
	if (!nrf54_uart.up || nrf54_uart.midi_len == LINK_PAYLOAD_MAX) {
		nrf54_uart.dropped++;
		return;
	}
	nrf54_uart.midi[nrf54_uart.midi_len++] = byte;
}

//
// A whole command for the radio, from something that is not the send
// queue: the commands the radio answers itself, from USB or from the
// pedal.  It waits for the control stream's window, behind any command
// before it, and is dropped only if the queue of them is full.
//
static void nrf54_uart_write(const uint8_t *buf, size_t len)
{
	uint16_t used = nrf54_uart.ctl_head - nrf54_uart.ctl_tail;

	if (!nrf54_uart.up || len > 255 ||
	    len + 1 > NRF54_CTL_RING_SIZE - used) {
		nrf54_uart.dropped++;
		return;
	}
	nrf54_uart.ctl[nrf54_uart.ctl_head++ & NRF54_CTL_RING_MASK] = len;
	for (size_t i = 0; i < len; i++)
		nrf54_uart.ctl[nrf54_uart.ctl_head++ & NRF54_CTL_RING_MASK] =
			buf[i];
}

//
// Packets into the ring, and the next run of the ring to the transmit DMA
// if it has finished the last.
//
// One contiguous span at a time, because the ring wraps and the DMA does
// not: what is left after the end of the buffer goes on the next call.
//
static void nrf54_uart_push(void)
{
	uint16_t head, span;

	nrf54_link_out();

	if (dma_channel_is_busy(nrf54_uart.dma_tx))
		return;

	// Nothing reuses what the DMA read until it has finished reading
	__dmb();
	nrf54_uart.tx_tail = (nrf54_uart.tx_tail + nrf54_uart.tx_inflight) &
			     NRF54_TX_RING_MASK;
	nrf54_uart.tx_bytes += nrf54_uart.tx_inflight;
	nrf54_uart.tx_inflight = 0;

	head = nrf54_uart.tx_head;
	if (head == nrf54_uart.tx_tail)
		return;

	span = head > nrf54_uart.tx_tail ? head - nrf54_uart.tx_tail
					 : NRF54_TX_RING_SIZE -
					   nrf54_uart.tx_tail;

	nrf54_uart.tx_inflight = span;
	// Every byte of the span is in memory before the DMA is started
	__dmb();
	dma_channel_set_read_addr(nrf54_uart.dma_tx,
				  &nrf54_uart.tx[nrf54_uart.tx_tail], false);
	dma_channel_set_trans_count(nrf54_uart.dma_tx, span, true);
}

//
// The pairing window.
//
// A bond is only accepted while the pedal says so, and the pedal is the
// only thing that can say it.  What opens it is a request over the cable,
// or one over the air from a device that is already bonded; being in
// range is not enough for either.
//
// Sixty seconds is long enough to find the menu on a phone and short
// enough that a pedal left on a stand is not offering itself to the room.
//
#define NRF54_PAIRING_MS	60000

static bool nrf54_pairing;
static uint32_t nrf54_pairing_until;

//
// Tell the radio which it is.  F0 7D 17, one byte, and the radio answers
// F0 7D 18 when a bond completes.
//
static void nrf54_pairing_tell(bool on)
{
	const uint8_t msg[] = { 0xF0, 0x7D, 0x17, on, 0xF7 };

	nrf54_uart_write(msg, sizeof(msg));
}

//
// Forget every Bluetooth key the radio holds.
//
// A peer cannot replace its own bond by asking - that would be an
// address anybody can copy - so a host that has forgotten its side needs
// the radio told, and the only thing that can tell it is a cable.
//
static void nrf54_forget_bonds(void)
{
	const uint8_t msg[] = { 0xF0, 0x7D, 0x19, 0xF7 };

	nrf54_uart_write(msg, sizeof(msg));
}

//
// Tell the radio what to advertise as: "Pedal" and the last four hex
// digits of the chip's unique id, the current board's USB name, so that
// pedals in one Bluetooth scan can be told apart.
//
// Sent whenever the radio says hello, which it does each time it starts.
//
#define NRF54_CMD_NAME	0x1f

static void nrf54_name_tell(void)
{
	static const char prefix[] = "Pedal ";
	char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
	uint8_t msg[3 + sizeof(prefix) - 1 + 4 + 1];
	size_t n = 0, len;

	pico_get_unique_board_id_string(id, sizeof(id));
	len = strlen(id);

	msg[n++] = 0xF0;
	msg[n++] = 0x7D;
	msg[n++] = NRF54_CMD_NAME;
	for (size_t i = 0; i < sizeof(prefix) - 1; i++)
		msg[n++] = prefix[i];
	for (size_t i = len - 4; i < len; i++)
		msg[n++] = id[i];
	msg[n++] = 0xF7;

	nrf54_uart_write(msg, n);
}

//
// Is the SysEx being handled right now one that arrived from the radio?
//
// handle_sysex_payload() is reached from both transports and the
// message looks the same either way, so the commands that are *about*
// the radio - a scan, its results - would otherwise be sent back where
// they came from.  Set around the call rather than passed through it,
// because every other caller would have to carry an argument it does
// not use.
//
static bool sysex_from_radio;

//
// One byte of MIDI from the radio.
//
static void nrf54_midi_in(struct midi_parser *parser, uint8_t b)
{
	uint8_t packet[4];

	if (!midi_parse_byte(parser, b, packet))
		return;

	nrf54_uart.packets++;

	//
	// What a footswitch or an editor played, on the debug port: channel
	// messages, Code Index Number 0x8 to 0xE.
	//
	if ((packet[0] & 0x0f) >= 0x8 && (packet[0] & 0x0f) <= 0xe) {
		dbg_puts("radio: ");
		dbg_hex(packet + 1, midi_cin_length(packet[0] & 0x0f));
		dbg_puts("\n");
	}

	sysex_from_radio = true;
	if (!handle_midi_packet(packet))
		usb_midi_write(packet);
	sysex_from_radio = false;
}

//
// The radio has started, for the first time or again.  Every stream begins
// afresh, half a message from before is nothing, and the radio is told
// what it needs to know about the pedal, since it knows nothing.
//
static void nrf54_link_hello(void)
{
	memset(&nrf54_uart.parser, 0, sizeof(nrf54_uart.parser));
	memset(&nrf54_uart.ctl_parser, 0, sizeof(nrf54_uart.ctl_parser));
	nrf54_uart.parser.want_sysex = true;
	nrf54_uart.ctl_parser.want_sysex = true;
	nrf54_uart.midi_len = 0;
	nrf54_uart.ctl_sent = 0;

	// Nothing has subscribed to a radio that has just started
	nrf54_uart.listening = false;

	//
	// A zero first: the radio's decoder believes nothing until it has
	// seen one, and anything sent before this may have reached it while
	// it was still in reset, so without it the first packet after the
	// hello is lost.
	//
	{
		const uint8_t sync = 0;

		nrf54_uart_put(&sync, 1);
	}

	nrf54_name_tell();
	nrf54_pairing_tell(nrf54_pairing);
}

//
// A packet from the radio that was dropped, on the debug port as it came
// off the wire, and where in the byte stream it ended, for finding what
// the losses have in common.
//
static void nrf54_link_failed(const struct link_rx *rx)
{
	dbg_puts(rx->failed == LINK_FAILED_CRC ? "link: dropped, CRC, " :
						  "link: dropped, malformed, ");
	dbg_dec(rx->wire_len);
	dbg_puts(" bytes, ending at byte ");
	dbg_dec(nrf54_uart.rx_bytes);
	dbg_puts("\nlink:   ");
	dbg_hex(rx->wire, rx->wire_len);
	dbg_puts("\n");
}

//
// Drain whatever the radio has sent, a packet at a time.
//
// The DMA has already put the bytes in memory, so this reads a pointer
// and walks what is new.  It needs no iteration count: the ring holds
// NRF54_RX_RING_SIZE bytes, so that is the most one pass can find however much
// noise the line is carrying.  A loop over the UART itself would need one,
// because an undriven receive pin delivers framing errors without end
// and would starve tud_task() until the board stopped enumerating.
//
static void nrf54_uart_poll(void)
{
	uint16_t head;

	if (!nrf54_uart.up)
		return;

	link_tick(&nrf54_uart.link, to_ms_since_boot(get_absolute_time()));

	// Say again how far every stream has got, in case an ack was lost
	if (to_ms_since_boot(get_absolute_time()) - nrf54_uart.acked_at >=
	    NRF54_ACK_AGAIN_MS) {
		link_ack_again(&nrf54_uart.link);
		nrf54_uart.acked_at = to_ms_since_boot(get_absolute_time());
	}

	nrf54_uart_push();

	head = nrf54_rx_head();
	while (nrf54_uart.rx_tail != head) {
		struct link *l = &nrf54_uart.link;
		uint8_t b = nrf54_uart.rx[nrf54_uart.rx_tail];

		nrf54_uart.rx_tail = (nrf54_uart.rx_tail + 1) &
				     NRF54_RX_RING_MASK;
		nrf54_uart.rx_bytes++;

		if (!link_rx_byte(&l->rx, b)) {
			if (l->rx.failed)
				nrf54_link_failed(&l->rx);
			continue;
		}

		switch (link_take(l)) {
		case LINK_GOT_HELLO:
			nrf54_link_hello();
			continue;
		case LINK_GOT_DATA:
			break;
		default:
			continue;
		}

		for (uint16_t i = LINK_HEADER; i < l->rx.len; i++) {
			uint8_t c = l->rx.buf[i];

			switch (l->rx.buf[0]) {
			case LINK_MIDI:
				nrf54_midi_in(&nrf54_uart.parser, c);
				break;
			case LINK_CONTROL:
				nrf54_midi_in(&nrf54_uart.ctl_parser, c);
				break;
			case LINK_DEBUG:
				// A line of the radio's printk
				if (i == LINK_HEADER &&
				    (l->rx.buf[3] & LINK_FIRST))
					dbg_puts("nrf54: ");
				dbg_write((const char *)&c, 1);
				break;
			}
		}
		link_consumed(l);
	}
}

//
// Bring the link up, with the radio held in reset while the pins change
// hands.
//
// The order is the whole of this function.  swd_init() drives
// NRF54_RTS low as a plain GPIO so that a radio released from reset is
// not mute; the moment the UART takes that pin it owns it, and a PL011
// with RTSEn not yet set deasserts RTS - drives it high - which would
// mute a running radio.  So reset goes back down first, the UART is
// configured whole, and only then is the radio let go.
//
// Called only when nrf54_probe() found a radio.  It leaves reset
// released, the state nrf54_probe() left it in.
//
static void nrf54_uart_init(void)
{
	gpio_put(NRF54_RESET, 0);

	uart_init(NRF54_UART, NRF54_UART_BAUD);
	uart_set_format(NRF54_UART, 8, 1, UART_PARITY_NONE);
	uart_set_hw_flow(NRF54_UART, LINK_FLOW_CONTROL, LINK_FLOW_CONTROL);

	//
	// FIFOs off, because the DMA is on.
	//
	// The PL011 raises its DMA request from the FIFO level, so with
	// FIFOs enabled the tail of a burst sits below the threshold
	// waiting for bytes that never come.  With them off, one byte is
	// one request.
	//
	uart_set_fifo_enabled(NRF54_UART, false);

	gpio_set_function(NRF54_TX, NRF54_UART_FUNCSEL);
	gpio_set_function(NRF54_RX, NRF54_UART_FUNCSEL);
#if LINK_FLOW_CONTROL
	gpio_set_function(NRF54_CTS, NRF54_UART_FUNCSEL);
	gpio_set_function(NRF54_RTS, NRF54_UART_FUNCSEL);
#endif

	//
	// An idle UART line is high, and this one is undriven for as long
	// as the radio takes to boot and bring its own UART up.  The pad
	// comes out of reset with its pull-*down* on, which holds the line
	// at what a receiver reads as a start bit that never ends - a
	// break, delivered as bytes for as long as nothing drives it.
	//
	// The pull-up holds it at idle instead, so a radio that never
	// answers delivers nothing rather than an endless break.
	//
	// CTS has the same pull-down and the opposite problem: low is
	// "clear to send", so while the radio boots with its RTS undriven,
	// whatever is sent goes into a UART that is not listening yet.  The
	// pull-up holds it at "not clear" until the radio drives it.
	//
	gpio_pull_up(NRF54_RX);
	gpio_pull_up(NRF54_CTS);

	//
	// SysEx, because the web app's protocol is built on it.  The TRS
	// jacks leave it off; this link is 32 times faster and carries it.
	//
	nrf54_uart.parser.want_sysex = true;
	nrf54_uart.ctl_parser.want_sysex = true;
	link_init(&nrf54_uart.link);

	//
	// Receive first and never stopped: 0xffffffff transfers into a
	// ring that wraps on its own address bits, so there is no
	// completion, no restart and nothing to miss.  CTS is honoured by
	// the UART itself, so the transmit side stalls in hardware when
	// the radio says stop.
	//
	nrf54_uart.dma_rx = dma_claim_unused_channel(true);
	dma_channel_config rx = dma_channel_get_default_config(nrf54_uart.dma_rx);
	channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
	channel_config_set_read_increment(&rx, false);
	channel_config_set_write_increment(&rx, true);
	channel_config_set_dreq(&rx, uart_get_dreq(NRF54_UART, false));
	channel_config_set_ring(&rx, true, NRF54_RX_RING_SHIFT);
	dma_channel_configure(nrf54_uart.dma_rx, &rx, nrf54_uart.rx,
			      &uart_get_hw(NRF54_UART)->dr, 0xffffffff, true);

	nrf54_uart.dma_tx = dma_claim_unused_channel(true);
	dma_channel_config tx = dma_channel_get_default_config(nrf54_uart.dma_tx);
	channel_config_set_transfer_data_size(&tx, DMA_SIZE_8);
	channel_config_set_read_increment(&tx, true);
	channel_config_set_write_increment(&tx, false);
	channel_config_set_dreq(&tx, uart_get_dreq(NRF54_UART, true));
	dma_channel_configure(nrf54_uart.dma_tx, &tx,
			      &uart_get_hw(NRF54_UART)->dr, nrf54_uart.tx,
			      0, false);

	nrf54_uart.up = true;

	busy_wait_us_32(1000);
	gpio_put(NRF54_RESET, 1);
}

#endif /* NRF54_SWDIO */
#endif /* NRF54_UART_H */
