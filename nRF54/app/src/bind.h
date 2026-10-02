#ifndef PEDAL_BIND_H
#define PEDAL_BIND_H

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

/*
 * The controller the player picked out of a scan.  Connecting to it,
 * subscribing to its MIDI characteristic, and reconnecting when it
 * comes back.
 */

#ifdef CONFIG_BT_CENTRAL

void bind_to(const bt_addr_le_t *addr);

/* MIDI bytes to the bound device; at most this many in one call */
#define BIND_SEND_MAX	8
int bind_send(const uint8_t *midi, size_t len);

#define BIND_CONNECTED	0x01
#define BIND_ENCRYPTED	0x02
#define BIND_FOUND	0x04
#define BIND_SUBSCRIBED	0x08
uint8_t bind_state(void);
int bind_last_err(void);

/*
 * Both roles share one set of connection callbacks, so midi.c asks
 * whose a connection is before treating it as the web app's.
 */
bool bind_owns(struct bt_conn *conn);
void bind_connected(struct bt_conn *conn, uint8_t err);
void bind_disconnected(struct bt_conn *conn, uint8_t reason);
void bind_encrypted(struct bt_conn *conn, bt_security_t level, int err);

#else

static inline bool bind_owns(struct bt_conn *conn) { return false; }
static inline uint8_t bind_state(void) { return 0; }
static inline int bind_last_err(void) { return 0; }

#endif /* CONFIG_BT_CENTRAL */

#endif /* PEDAL_BIND_H */
