#ifndef PEDAL_MIDI_H
#define PEDAL_MIDI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * The line between the two halves of the bridge: main.c owns the UART,
 * midi.c owns Bluetooth, and neither reaches past this.
 */

/* main.c, called by midi.c when a packet arrives over the air */
void midi_uart_send(const uint8_t *buf, size_t len);
void midi_ble_queue(const uint8_t *buf, uint16_t len);

/* main.c: how many bytes from the pedal are waiting to be dealt with */
uint32_t midi_uart_backlog(void);

/* main.c: true while the pedal is being held off */
bool midi_uart_halted(void);

/* main.c: bytes the pedal sent that there was no room for */
uint32_t midi_uart_lost(void);


void midi_ble_controller(const uint8_t *buf, uint16_t len);

#ifdef CONFIG_BT

/* midi.c, fed by main.c from the UART */
void midi_ble_start(void);
void midi_ble_feed(uint8_t byte);
void midi_ble_flush(void);
void midi_ble_packet(const uint8_t *buf, uint16_t len);
bool midi_ble_ready(void);

#else

/*
 * The nobt image builds midi.c out entirely - see app/CMakeLists.txt -
 * so the bridge becomes a UART that reads and discards.
 */
static inline void midi_ble_start(void) { }
static inline void midi_ble_feed(uint8_t byte) { (void)byte; }
static inline void midi_ble_flush(void) { }
static inline void midi_ble_packet(const uint8_t *b, uint16_t l)
					{ (void)b; (void)l; }
static inline bool midi_ble_ready(void) { return true; }

#endif /* CONFIG_BT */

#endif /* PEDAL_MIDI_H */
