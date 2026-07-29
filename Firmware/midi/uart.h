#if MIDI_HW
#define UART_TX_BUF_SIZE 512
static uint8_t uart_tx_buf[UART_TX_BUF_SIZE];
static volatile unsigned uart_tx_head;
static volatile unsigned uart_tx_tail;

#define UART_RX_BUF_SIZE 256
static uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
static volatile unsigned uart_rx_head;
static volatile unsigned uart_rx_tail;
#endif

void uart_midi_write(const uint8_t packet[4])
{
#if MIDI_HW
	int len = midi_cin_length(packet[0] & 0x0F);

	for (int i = 0; i < len; i++) {
		unsigned head = uart_tx_head;
		unsigned next_head = (head + 1) % UART_TX_BUF_SIZE;
		if (next_head != uart_tx_tail) {
			uart_tx_buf[head] = packet[1 + i];
			uart_tx_head = next_head;
		}
	}
#endif
}

bool uart_midi_read(uint8_t packet[4])
{
#if MIDI_HW
	static struct midi_stream_parser parser;

	while (uart_rx_head != uart_rx_tail) {
		uint8_t b = uart_rx_buf[uart_rx_tail];
		uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;

		if (midi_stream_read(&parser, b, packet))
			return true;
	}
#endif
	return false;
}

void uart_midi_poll(void)
{
#if MIDI_HW
	while (uart_is_readable(MIDI_UART)) {
		unsigned head = uart_rx_head;
		unsigned next_head = (head + 1) % UART_RX_BUF_SIZE;
		if (next_head != uart_rx_tail) {
			uart_rx_buf[head] = uart_getc(MIDI_UART);
			uart_rx_head = next_head;
		} else {
			break;
		}
	}

	while (uart_tx_head != uart_tx_tail && uart_is_writable(MIDI_UART)) {
		uart_putc_raw(MIDI_UART, uart_tx_buf[uart_tx_tail]);
		uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
	}

	uint8_t packet[4];
	while (uart_midi_read(packet)) {
		if (!handle_midi_packet(packet)) {
			usb_midi_write(packet); // MIDI Thru: Echo to USB if not for us
			uart_midi_write(packet); // MIDI Thru: Echo to UART if not for us
		}
	}
#endif
}

static void uart_midi_init(void)
{
#if MIDI_HW
	// The funcsel is a property of the pins and comes from
	// board.h.  UART_FUNCSEL_NUM() can't work it out, and the
	// answer is not the same on both boards: UART1 TX/RX is
	// function 2 on GPIO 20/21 and function 11 on 26/27,
	// because 26/27 have CTS/RTS at 2 and the TX/RX pair is
	// one of the RP2350's extended functions.
	//
	// Don't even ask how long it took to debug this the first
	// time: I had read the datasheet when setting this all up,
	// but I hadn't connected the dots on UART_FUNCSEL_NUM() not
	// doing the right thing.  Then it was hardcoded to 11 here,
	// which cost a second evening on the board that wants 2 -
	// the pins silently ended up muxed to a function that does
	// not exist on them, which is as quiet as a failure gets.
	gpio_set_function(MIDI_OUT, MIDI_FUNCSEL);
	gpio_set_function(MIDI_IN, MIDI_FUNCSEL);

	// MIDI idle is +5V, but that is "no current": LED is off,
	// and the TLP2310 drives the MIDI_IN pin low.
	//
	// Standard UART idle is high, but that is easily dealt
	// with by just inverting the GPIO pin
	gpio_set_inover(MIDI_IN, GPIO_OVERRIDE_INVERT);

	// Let it rip!
	uart_init(MIDI_UART, 31250);
#endif
}
