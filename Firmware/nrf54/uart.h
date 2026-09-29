#ifndef NRF54_UART_H
#define NRF54_UART_H

#ifdef NRF54_SWDIO

//
// The command link to the radio, which is a MIDI port and nothing else.
//
// Bytes go out, bytes come in, and nothing here knows what carries them
// afterwards: the packet format, the timestamps and the Bluetooth
// attribute they are written to are all the radio's business.
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
// The receive ring is a power of two and aligned to its own size
// because the DMA wraps it with an address mask.  The transmit ring is
// walked in software and needs neither.
//
#define NRF54_TX_RING		1024
#define NRF54_RX_RING		1024
#define NRF54_RX_RING_BITS	10

static struct {
	uint8_t tx[NRF54_TX_RING];
	uint16_t tx_head, tx_tail;
	uint16_t tx_inflight;		// handed to the DMA, not yet gone

	uint8_t rx[NRF54_RX_RING] __attribute__((aligned(NRF54_RX_RING)));
	uint16_t rx_tail;

	int dma_tx, dma_rx;

	struct midi_parser parser;

	uint32_t tx_bytes, rx_bytes;
	uint32_t packets, dropped;
	bool up;
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

	return (uint16_t)(at - (uintptr_t)nrf54_uart.rx) % NRF54_RX_RING;
}

//
// Is there room for another byte on its way to the radio?
//
// Asked by midi_tx_to_radio() before it takes a byte off a message, so
// that a full queue pauses this consumer rather than losing the middle of
// a SysEx.  Half full rather than full: a message is taken apart a byte
// at a time and the caller checks once per byte, so stopping at the
// half-way mark leaves room for the burst already in flight.
//
// It is the last stage of a chain that starts at the far end.  A
// Bluetooth Low Energy client that cannot keep up stops the radio
// draining its receive ring; the ring deasserts RTS; this pedal's
// transmitter stalls on CTS and its queue fills, and then this returns
// false.  Each stage stops the one before it, so a schema of tens of
// kilobytes crosses as one message instead of being cut short - and it
// stops this consumer only, which is what the cursors are for.
//
static bool nrf54_uart_ready(void)
{
	uint16_t used = (nrf54_uart.tx_head - nrf54_uart.tx_tail) %
			NRF54_TX_RING;

	return used < NRF54_TX_RING / 2;
}

// Defined below, and declared here because the senders above it call it.
static void nrf54_uart_write(const uint8_t *buf, size_t len);

//
// One outgoing MIDI byte, on its way to the radio.
//
// Called from midi_tx_to_radio(), which walks the send queue with a
// cursor of its own, so falling behind here costs USB nothing.  Declared
// in midi/tx.h because that file comes first; see the note there.
//
// It queues rather than writes.  midi_tx_drain() empties a whole message
// in a tight loop, so a state dump arrives here as several hundred bytes
// in a few microseconds, while the wire carries one byte per 10 us - the
// transmit FIFO is 32 deep and a burst would lose everything past it.
// Queuing here and draining from the main loop is what makes the link
// keep up with MIDI rather than with the loop that generates it.
//
// A full ring drops rather than blocking, because the caller has no way
// to wait.  With nrf54_uart_ready() honoured above it that cannot happen,
// so 'dropped' reading non-zero means somebody added a caller that does
// not ask first.
//
static void nrf54_uart_thru(uint8_t byte)
{
	if (!nrf54_uart.up)
		return;

	uint16_t next = (nrf54_uart.tx_head + 1) % NRF54_TX_RING;
	if (next == nrf54_uart.tx_tail) {
		nrf54_uart.dropped++;
		return;
	}

	nrf54_uart.tx[nrf54_uart.tx_head] = byte;
	nrf54_uart.tx_head = next;
}

//
// Hand the transmit DMA the next run of bytes, if it has finished the
// last.
//
// One contiguous span at a time, because the ring wraps and the DMA does
// not: what is left after the end of the buffer goes on the next call.
// The bytes it is working on stay counted in the ring until it is done,
// so nrf54_uart_ready() sees them and the back-pressure above still
// measures the real backlog.
//
//
// A whole message to the radio, from something that is not the send queue.
//
// Used for the commands the radio answers itself, which arrive over USB
// and have to reach it without being treated as MIDI on the way.
//
static void nrf54_uart_write(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		nrf54_uart_thru(buf[i]);
}

static void nrf54_uart_push(void)
{
	uint16_t head, span;

	if (dma_channel_is_busy(nrf54_uart.dma_tx))
		return;

	nrf54_uart.tx_tail = (nrf54_uart.tx_tail + nrf54_uart.tx_inflight) %
			     NRF54_TX_RING;
	nrf54_uart.tx_bytes += nrf54_uart.tx_inflight;
	nrf54_uart.tx_inflight = 0;

	head = nrf54_uart.tx_head;
	if (head == nrf54_uart.tx_tail)
		return;

	span = head > nrf54_uart.tx_tail ? head - nrf54_uart.tx_tail
					 : NRF54_TX_RING - nrf54_uart.tx_tail;

	nrf54_uart.tx_inflight = span;
	dma_channel_set_read_addr(nrf54_uart.dma_tx,
				  &nrf54_uart.tx[nrf54_uart.tx_tail], false);
	dma_channel_set_trans_count(nrf54_uart.dma_tx, span, true);
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
// Drain whatever the radio has sent, and treat it as MIDI.
//
// The DMA has already put the bytes in memory, so this reads a pointer
// and walks what is new.  It needs no iteration count: the ring holds
// 1024 bytes, so that is the most one pass can find however much noise
// the line is carrying.  A loop over the UART itself would need one,
// because an undriven receive pin delivers framing errors without end
// and would starve tud_task() until the board stopped enumerating.
//
static void nrf54_uart_poll(void)
{
	uint16_t head;

	if (!nrf54_uart.up)
		return;

	nrf54_uart_push();

	head = nrf54_rx_head();
	while (nrf54_uart.rx_tail != head) {
		uint8_t packet[4];
		uint8_t b = nrf54_uart.rx[nrf54_uart.rx_tail];

		nrf54_uart.rx_tail = (nrf54_uart.rx_tail + 1) % NRF54_RX_RING;
		nrf54_uart.rx_bytes++;

		if (!midi_parse_byte(&nrf54_uart.parser, b, packet))
			continue;

		nrf54_uart.packets++;

		sysex_from_radio = true;
		if (!handle_midi_packet(packet))
			usb_midi_write(packet);
		sysex_from_radio = false;
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
	uart_set_hw_flow(NRF54_UART, true, true);

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
	gpio_set_function(NRF54_CTS, NRF54_UART_FUNCSEL);
	gpio_set_function(NRF54_RTS, NRF54_UART_FUNCSEL);

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
	gpio_pull_up(NRF54_RX);

	//
	// SysEx, because the web app's protocol is built on it.  The TRS
	// jacks leave it off; this link is 32 times faster and carries it.
	//
	nrf54_uart.parser.want_sysex = true;

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
	channel_config_set_ring(&rx, true, NRF54_RX_RING_BITS);
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
