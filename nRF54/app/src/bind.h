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

#endif /* CONFIG_BT_CENTRAL */

#endif /* PEDAL_BIND_H */
