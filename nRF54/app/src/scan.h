#ifndef PEDAL_SCAN_H
#define PEDAL_SCAN_H

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/bluetooth/bluetooth.h>

/* As much of a device's name as a result carries. */
#define SCAN_NAME_MAX		24

/*
 * One pass over what is advertising nearby, so that a person can pick
 * their controller out of the list by name.  Nothing is connected to:
 * an advertisement does not say whether a device speaks MIDI, and the
 * player knows which one is theirs.
 */

#ifdef CONFIG_BT_OBSERVER

/* Start one pass.  Results arrive through the two below. */
void scan_start(void);
void scan_stop(void);

/* Defined in midi.c: one device, once the pass is over. */
void scan_found(const bt_addr_le_t *addr, const char *name);

/* Defined in midi.c: the pass is over, and how many it listed. */
void scan_done(unsigned int listed);

#else

static inline void scan_start(void) { }
static inline void scan_stop(void) { }

#endif /* CONFIG_BT_OBSERVER */

#endif /* PEDAL_SCAN_H */
