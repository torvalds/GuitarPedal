/*
 * The UART side of the bridge: uart30 carries a plain MIDI byte stream
 * to and from the RP2354.  midi.c is the Bluetooth side.
 *
 * It carries nothing else.  There is no console anywhere on this radio -
 * prj.conf says why.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "midi.h"

/*
 * Reading RTT stalls the core long enough to miss Bluetooth connection
 * events, so a log kept over the debug pins breaks the link it is there
 * to explain - 6 to 20 disconnects per run against 0 or 1 without it.
 * prj.conf turns the console off entirely; this is what stops anything
 * turning that one back on quietly.
 */
#ifdef CONFIG_USE_SEGGER_RTT
#error "RTT costs Bluetooth connections - see prj.conf"
#endif

static const struct device *const link =
	DEVICE_DT_GET(DT_CHOSEN(pedal_midi_uart));

/*
 * Buffers for the asynchronous UART API - see prj.conf for why it is
 * that one.
 *
 * The timeout is what stops a short message waiting for a full buffer:
 * without it a three-byte controller change sits here until 256 bytes
 * have arrived, which on a quiet link is never.
 */
#define RX_BUFS		2
#define RX_BUF_LEN	256
#define RX_TIMEOUT_US	200	/* 20 byte-times at 1 Mbit */

static uint8_t rx_buf[RX_BUFS][RX_BUF_LEN];

/*
 * Which buffers the driver does not currently hold.
 *
 * A flag each rather than an index of the last one released: the driver
 * asks for the next buffer when it starts using one, not when it gives
 * one back, so the two events do not alternate and an index is wrong as
 * soon as two requests arrive together.  Handing out a buffer that is
 * already being filled loses whatever was in it.
 */
static bool rx_free[RX_BUFS];

static uint8_t *rx_take(void)
{
	for (int i = 0; i < RX_BUFS; i++) {
		if (rx_free[i]) {
			rx_free[i] = false;
			return rx_buf[i];
		}
	}
	return NULL;
}

/*
 * A ring each way: the driver fills one and empties the other, and the
 * loop below does the opposite.
 *
 * The receive ring is the one that has to be sized.  A SysEx schema is
 * tens of kilobytes arriving at 1 Mbit while Bluetooth carries it away
 * more slowly, and the difference collects here.  rx_starved() is what
 * happens when it fills.
 */
RING_BUF_DECLARE(uart_rx_ring, 2048);
RING_BUF_DECLARE(uart_tx_ring, 1024);

static bool tx_busy;
static uint32_t tx_claimed;
static bool rx_stopped;

/*
 * Bytes the pedal sent that there was no room for.
 *
 * ring_buf_put() takes what fits and says how much, and ignoring that is
 * how a byte disappears with nobody the wiser.
 */
static uint32_t rx_lost;

/*
 * Is there room for another buffer's worth?
 *
 * Answering "no" is how RTS gets deasserted: the driver is refused a
 * buffer, the current one runs out, and a UARTE with nowhere to put the
 * next byte stops the far end instead of losing it.  Discarding here
 * would drop most of a schema and report nothing.
 *
 * Room for every buffer the driver may still be holding, not just for
 * one: it has one active and one already queued when it requests the
 * next, so up to RX_BUFS bufferfuls can still arrive after the
 * refusal.
 */
static bool rx_starved(void)
{
	return ring_buf_space_get(&uart_rx_ring) < RX_BUFS * RX_BUF_LEN;
}

static void tx_kick(void)
{
	uint8_t *data;
	uint32_t len;

	if (tx_busy)
		return;

	len = ring_buf_get_claim(&uart_tx_ring, &data, UINT32_MAX);
	if (!len)
		return;

	tx_busy = true;
	tx_claimed = len;
	uart_tx(link, data, len, SYS_FOREVER_US);
}

static void uart_cb(const struct device *dev, struct uart_event *evt,
		    void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_RX_RDY: {
		uint32_t took = ring_buf_put(&uart_rx_ring,
					     evt->data.rx.buf +
					     evt->data.rx.offset,
					     evt->data.rx.len);

		if (took < evt->data.rx.len)
			rx_lost += evt->data.rx.len - took;
		break;
	}

	case UART_RX_BUF_REQUEST: {
		uint8_t *buf;

		if (rx_starved())
			break;
		buf = rx_take();
		if (buf)
			uart_rx_buf_rsp(dev, buf, RX_BUF_LEN);
		break;
	}

	case UART_RX_BUF_RELEASED:
		for (int i = 0; i < RX_BUFS; i++)
			if (evt->data.rx_buf.buf == rx_buf[i])
				rx_free[i] = true;
		break;

	case UART_RX_DISABLED:
		rx_stopped = true;
		break;

	case UART_TX_DONE:
	case UART_TX_ABORTED:
		ring_buf_get_finish(&uart_tx_ring, tx_claimed);
		tx_claimed = 0;
		tx_busy = false;
		break;

	default:
		break;
	}
}

/*
 * MIDI bytes for the RP2354, from whatever arrived over Bluetooth.
 *
 * Called from the Bluetooth stack's context, so it queues rather than
 * writes: the driver does the writing.
 */
void midi_uart_send(const uint8_t *buf, size_t len)
{
	ring_buf_put(&uart_tx_ring, buf, len);
	tx_kick();
}

/*
 * How much the pedal has sent that has not been dealt with yet, and
 * whether it is being held off.
 *
 * Read by the radio's statistics message.  A backlog that is not
 * draining is what refuses the driver a buffer and deasserts RTS, so
 * between them the two say whether the pedal has been stopped, is
 * keeping up, or is simply not sending.
 */
uint32_t midi_uart_backlog(void)
{
	return ring_buf_size_get(&uart_rx_ring);
}

bool midi_uart_halted(void)
{
	return rx_stopped;
}

uint32_t midi_uart_lost(void)
{
	return rx_lost;
}

int main(void)
{
	if (!device_is_ready(link))
		return -ENODEV;

	uart_callback_set(link, uart_cb, NULL);
	for (int i = 1; i < RX_BUFS; i++)
		rx_free[i] = true;
	uart_rx_enable(link, rx_buf[0], RX_BUF_LEN, RX_TIMEOUT_US);

	midi_ble_start();

	for (;;) {
		uint8_t *buf;
		uint32_t n, took = 0;

		/*
		 * Bytes from the pedal, and only as many as the radio can
		 * take right now: midi_ble_feed() may have to send a
		 * packet to make room, and checking first is what stops it
		 * having to wait.  What is left stays in the ring, which
		 * fills, which deasserts RTS and pauses the pedal.
		 *
		 * One free buffer per byte is enough, and that is the
		 * invariant the whole arrangement rests on.  A single byte
		 * can fill the packet being built and so send one - and
		 * once sent, the next packet has the whole MTU free, which
		 * is far more than the handful of bytes any one call still
		 * has to write.  So no call needs two.
		 */
		n = ring_buf_get_claim(&uart_rx_ring, &buf, UINT32_MAX);
		while (took < n && midi_ble_ready())
			midi_ble_feed(buf[took++]);
		ring_buf_get_finish(&uart_rx_ring, took);

		if (took)
			continue;

		/*
		 * Nothing waiting, so whatever is half-packed is as
		 * complete as it is going to get: send it rather than
		 * hold it for company.  This is what batches a burst into
		 * full packets and still gets a lone note out at once.
		 *
		 * Only when there is somewhere for it to go.  A flush that
		 * cannot send discards, so checking first is the difference
		 * between waiting a pass and losing the packet.
		 */
		if (midi_ble_ready())
			midi_ble_flush();

		/*
		 * And if the ring filled far enough to stop the far end,
		 * it has now drained: start listening again.
		 */
		if (rx_stopped && !rx_starved()) {
			uint8_t *buf = rx_take();

			if (buf) {
				rx_stopped = false;
				uart_rx_enable(link, buf, RX_BUF_LEN,
					       RX_TIMEOUT_US);
			}
		}

		tx_kick();
		k_sleep(K_MSEC(1));
	}
}
