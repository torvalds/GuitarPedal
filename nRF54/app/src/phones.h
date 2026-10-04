#ifndef PEDAL_PHONES_H
#define PEDAL_PHONES_H

#include <stdbool.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

/*
 * phones.c: LE Audio headphones, sent what the pedal sends.  Like
 * bind.c's controller, a connection this end opens, so midi.c asks whose
 * a connection is before treating it as a host's.
 */
#ifdef CONFIG_BT_BAP_UNICAST_CLIENT
int phones_start(void);
void phones_to(const bt_addr_le_t *addr);
bool phones_owns(struct bt_conn *conn);
void phones_connected(struct bt_conn *conn, uint8_t err);
void phones_disconnected(struct bt_conn *conn, uint8_t reason);
void phones_encrypted(struct bt_conn *conn, bt_security_t level, int err);
#else
static inline int phones_start(void) { return 0; }
static inline bool phones_owns(struct bt_conn *conn) { return false; }
#endif

#endif /* PEDAL_PHONES_H */
