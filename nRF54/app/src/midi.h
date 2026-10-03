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

/* main.c: a whole answer for the pedal, on the control stream */
void midi_uart_control(const uint8_t *msg, size_t len);
void midi_ble_queue(uint8_t src, uint8_t peer, uint8_t flags,
		    const uint8_t *buf, uint16_t len);

/* midi_ble_queue()'s flags: the peer is one the pedal may trust, the same
 * bit as LINK_TRUSTED in link.h */
#define MIDI_FROM_TRUSTED	0x04

/* main.c: how many bytes from the pedal are waiting to be dealt with */
uint32_t midi_uart_backlog(void);

/* main.c: bytes from the pedal overwritten before they were read */
uint32_t midi_uart_lost(void);

/* main.c: the packet link's counts */
void midi_uart_link_counts(uint32_t *gaps, uint32_t *refused, uint32_t *bad,
			   uint32_t *lost);
uint32_t midi_uart_written_off(void);
uint32_t midi_uart_crc_failed(void);
uint32_t midi_uart_received(void);


struct bt_conn;
void midi_ble_controller(struct bt_conn *conn, const uint8_t *buf,
			 uint16_t len);

#ifdef CONFIG_BT

/* midi.c, fed by main.c from the UART */
void midi_ble_start(void);
void midi_ble_feed(uint8_t byte);
void midi_ble_flush(void);
void midi_ble_packet(uint8_t src, const uint8_t *buf, uint16_t len);
void midi_ble_to(uint8_t peer);
void midi_ble_resync(void);
#define MIDI_QUIET_UNICAST	1	/* a phone is streaming */
#define MIDI_QUIET_PHONES	2	/* so are headphones */
void midi_ble_quiet(unsigned int why, bool quiet);
bool midi_ble_ready(void);
void midi_ble_notices(void);
void midi_radio_command(const uint8_t *msg, size_t len);

#else

/*
 * The nobt image builds midi.c out entirely - see app/CMakeLists.txt -
 * so the bridge becomes a UART that reads and discards.
 */
static inline void midi_ble_start(void) { }
static inline void midi_ble_feed(uint8_t byte) { (void)byte; }
static inline void midi_ble_flush(void) { }
static inline void midi_ble_packet(uint8_t s, const uint8_t *b, uint16_t l)
					{ (void)s; (void)b; (void)l; }
static inline void midi_ble_to(uint8_t peer) { (void)peer; }
static inline void midi_ble_resync(void) { }
static inline bool midi_ble_ready(void) { return true; }
static inline void midi_ble_notices(void) { }
static inline void midi_radio_command(const uint8_t *m, size_t l)
					{ (void)m; (void)l; }

#endif /* CONFIG_BT */

#endif /* PEDAL_MIDI_H */
