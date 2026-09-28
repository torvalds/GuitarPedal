/*
 * The first thing to run on the radio, and deliberately the smallest:
 * it echoes whatever the RP2354 sends it.
 *
 * It also advertises, and that is the whole of what the radio does -
 * see midi.c.  A scanner can see the far side of the link and find the
 * characteristic the notes will eventually come out of; nothing is sent
 * and nothing arriving is acted on.
 *
 * What it proves is everything underneath - that the chip is running
 * its own image, that the board definition names the right pins, that
 * the low-frequency clock the board does not have a crystal for was not
 * needed to boot, and that the four wires of the command link are the
 * four wires we think they are.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

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

#ifdef CONFIG_BT
void midi_ble_start(void);
#else
#define midi_ble_start() do { } while (0)
#endif

static const struct device *const link =
	DEVICE_DT_GET(DT_CHOSEN(pedal_midi_uart));

int main(void)
{
	unsigned char c;

	if (!device_is_ready(link))
		return -ENODEV;

	midi_ble_start();

	for (;;) {
		if (uart_poll_in(link, &c) == 0)
			uart_poll_out(link, c);
		else
			k_sleep(K_MSEC(1));
	}
}
