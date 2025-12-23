#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"

#define STOMP_PIN	0
#define LED_PIN		1

// NOTE! The pots are reversed, so the 12-bit ADC read will read 4095 -> 0 clockwise
#define POT1		26
#define POT2		27
#define POT3		28
#define POT4		29

int main()
{
	gpio_init(STOMP_PIN);
	gpio_set_dir(STOMP_PIN, GPIO_IN);
	gpio_pull_up(STOMP_PIN);

	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, GPIO_OUT);

	adc_init();
	adc_gpio_init(POT1);
	adc_select_input(0);

	// Let's read the ADC's one at a time in order
	adc_set_round_robin(0xf);

	// This tests all four pots, the stomp switch, and the LED
	while (true) {
		int delay = 4095 & ~adc_read();

		delay /= 2;
		if (gpio_get(STOMP_PIN))
			delay /= 2;

		gpio_put(LED_PIN, 1);
		sleep_ms(delay);
		gpio_put(LED_PIN, 0);
		sleep_ms(delay);
	}
}
