/*
 * The first thing to run on the radio, and deliberately the smallest:
 * it says what it is and echoes whatever the RP2354 sends it.
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
 * Which of the two radio images this is - the one with the Bluetooth
 * stack, or the one without.  Nothing else tells them apart at runtime.
 */
#ifdef CONFIG_BT
#define RADIO_STACK "bt"
void midi_ble_start(void);
#else
#define RADIO_STACK "nobt"
#define midi_ble_start() do { } while (0)
#endif

static const struct device *const link =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

int main(void)
{
	unsigned char c;

	if (!device_is_ready(link))
		return -ENODEV;

	printk("radio: minimal/nrf54l10 " RADIO_STACK " "
	       __DATE__ " " __TIME__ "\n");

	midi_ble_start();

	for (;;) {
		if (uart_poll_in(link, &c) == 0)
			uart_poll_out(link, c);
		else
			k_sleep(K_MSEC(1));
	}
}
