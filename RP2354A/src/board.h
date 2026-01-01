//
// Board GPIO pin definitions and direct rp2354 ADC access
// 
#define STOMP_PIN	0
#define LED_PIN		1

#define I2C_SDA		4
#define I2C_SCL		5

#define I2S_BCLK	8
#define I2S_FSYNC	9
#define I2S_DIN		10
#define I2S_DOUT	11

// NOTE! The pots are reversed, so the 12-bit ADC read will read 4095 -> 0 clockwise
#define POT1		26
#define POT2		27
#define POT3		28
#define POT4		29

static inline float read_pot(void)
{
	return (4095 & ~adc_read()) * (1.0 / 4096);
}

#define PWM_100 4096
#define PWM_50 2048
