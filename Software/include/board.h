//
// Board GPIO pin definitions
//
// Named for what the pin does, not for the order someone happened to
// wire it up in.  The board has been through several generations and
// the old numbering survived none of them: "SW1" was the first rotary's
// shaft switch, "SW3" was the first stomp, and SW2/SW4 were a second
// rotary and a second stomp that no longer exist.  None of that was
// visible in the names.
//
// The pedal as it stands has one rotary encoder - and most boards do
// not even populate that - one stomp switch, and one LED.
//

// The one LED.  Plain PWM brightness; see set_led().
#define LED_GPIO		0

//
// The one rotary encoder: A and B are the quadrature pair, SW is the
// shaft pressing down.
//
#define ROTARY_A_GPIO		6
#define ROTARY_B_GPIO		7
#define ROTARY_SW_GPIO		12

// The one stomp switch.  Internal pull-up, closing to GND.
#define STOMP_GPIO		13

#define I2S_BCLK		8
#define I2S_FSYNC		9
#define I2S_DIN			10
#define I2S_DOUT		11

// Hardware MIDI TRS, on the pins the second rotary encoder used to have
#if MIDI_HW
  #define MIDI_OUT		26
  #define MIDI_IN		27
  #define MIDI_UART		uart1
#endif

#define I2C0_SDA		4
#define I2C0_SCL		5
#define I2C1_SDA		2
#define I2C1_SCL		3

#define MC24Cxx_I2C		i2c0, 0x50
#define TAC5112_I2C		i2c0, 0x51
#define SH1106_I2C		i2c1, 0x3c

//
// Not all boards have this.  It is the next generation's LED - a smart
// one where there is currently a plain one - and it wants GPIO1, which
// is free precisely because the second LED that used to be there is
// gone.
//
#if 0
#define WS2812_GPIO		1
#endif

//
// Pins that used to be something and no longer are, recorded so nobody
// has to go through the git history to find out why there are gaps:
//
//	GPIO 1		second LED (now WS2812_GPIO, above)
//	GPIO 26/27	second rotary's quadrature pair (now hardware MIDI)
//	GPIO 28		second rotary's shaft switch
//	GPIO 29		second stomp switch
//
