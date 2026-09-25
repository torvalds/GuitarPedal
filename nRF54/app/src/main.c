/*
 * Signs of life.
 *
 * The first thing to run on the radio, and deliberately the smallest:
 * it says what it is and echoes whatever the RP2354 sends it.  It never
 * advertises and never touches the i2s, though the radio stack is
 * linked in - see prj.conf for why, and for what it costs.
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
 * Which of the two images this is.  The banner is the only thing that
 * says so, and telling them apart is the whole point of having two.
 */
#ifdef CONFIG_BT
#define RADIO_STACK "bt"
#else
#define RADIO_STACK "nobt"
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

	for (;;) {
		if (uart_poll_in(link, &c) == 0)
			uart_poll_out(link, c);
		else
			k_sleep(K_MSEC(1));
	}
}
