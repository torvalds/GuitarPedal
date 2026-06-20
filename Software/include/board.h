//
// Board GPIO pin definitions
//

//
// New custom carrier board:
//  - WS2812 on GPIO1
//
// I2C0 on GPIO4/5 (and I2C1 on GPIO2/3)
//
// Foot (or finger) switches on GPIO 6/7/8. Internal
// pull-up with switch closing to GND.
//

#define GPIO_SW1		12
#define GPIO_SW2		28
#define GPIO_SW3		13
#define GPIO_SW4		29

#define PWM_PIN1		0
#define PWM_PIN2		1

#define I2S_BCLK		8
#define I2S_FSYNC		9
#define I2S_DIN			10
#define I2S_DOUT		11

#define GPIO_ROT1A		6
#define GPIO_ROT1B		7

// The MIDI-controlled ones do not have
// the second rotary encoder
#if MIDI_HW
  #define MIDI_OUT		26
  #define MIDI_IN		27
  #define MIDI_UART		uart1
#else
  #define GPIO_ROT2A		26
  #define GPIO_ROT2B		27
#endif


#define I2C0_SDA		4
#define I2C0_SCL		5
#define I2C1_SDA		2
#define I2C1_SCL		3

#define MC24C02_I2C		i2c0, 0x50
#define TAC5112_I2C		i2c0, 0x51
#define SH1106_I2C		i2c1, 0x3c

// Nor all boards have this
#if 0
#define WS2812_PIN		1	// WS2812 RGB LED
#endif
