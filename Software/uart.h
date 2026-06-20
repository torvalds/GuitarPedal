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
	uint8_t cin = packet[0] & 0x0F;
	int len = 0;
	switch (cin) {
		case 0x8: case 0x9: case 0xB: case 0xE: len = 3; break;
		case 0xC: case 0xD: len = 2; break;
		default: len = 0; break;
	}
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
	static int expected_bytes = 0;
	static uint8_t parser_packet[4];
	static int parser_idx = 0;

	while (uart_rx_head != uart_rx_tail) {
		uint8_t b = uart_rx_buf[uart_rx_tail];
		uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;

		if (b >= 0xF8) {
			// Real-time message
			continue;
		} else if (b >= 0x80) {
			parser_packet[1] = b;
			parser_idx = 2;
			if ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) {
				expected_bytes = 1;
			} else if (b < 0xF0) {
				expected_bytes = 2;
			} else {
				expected_bytes = 0;
			}
		} else if (expected_bytes > 0 && parser_idx > 0) {
			parser_packet[parser_idx++] = b;
			if (parser_idx - 2 == expected_bytes) {
				packet[0] = 0;
				packet[1] = parser_packet[1];
				packet[2] = parser_packet[2];
				packet[3] = parser_packet[3];
				parser_idx = 2;
				return true;
			}
		}
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
	// On RP2350, function 11 maps GPIO 26/27 to UART1 TX/RX
	// The normal UART_FUNCSEL_NUM macro can't deal with that
	//
	// Don't even ask how long it took to debug this: I had
	// read the datasheet when setting this all up, but I
	// hadn't connected the dots on UART_FUNCSEL_NUM() not
	// doing the right thing.
	//
	// There is probably some proper way to do this in the
	// SDK, but whatever.
	gpio_set_function(MIDI_OUT, 11);
	gpio_set_function(MIDI_IN, 11);

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
