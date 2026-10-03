/*
 * Where the radio's UART receive DMA has got to, from what the UARTE says.
 * main.c reads the registers; this is kept apart so that
 * Validation/test-uartrx.c can check it against a model of the hardware.
 *
 * Receive is one ring that the DMA refills from the top for ever: the END
 * to START shortcut restarts it, and its pointer is never moved.  Nothing
 * says where the DMA has got to while it runs, except that AMOUNT is also
 * updated on a match, and the match filter is set to the zero that ends
 * every packet.  So the loop knows up to the end of the last whole packet,
 * which is all the decoder needs.  END counts the wraps.
 *
 * Positions are bytes since the start, and wrap at 32 bits along with
 * everything that compares them.
 */
#ifndef UARTRX_H
#define UARTRX_H

#include <stdbool.h>
#include <stdint.h>

#include "link.h"

/* 4 kB is 40 ms at 1 Mbit */
#define UARTRX_SHIFT	12
#define UARTRX_SIZE	(1 << UARTRX_SHIFT)
#define UARTRX_MASK	(UARTRX_SIZE - 1)

struct uartrx {
	uint32_t wraps;
	uint32_t head;		/* bytes the DMA has written, to the last zero */
	uint32_t tail;		/* bytes taken */
	uint32_t lost;		/* bytes overwritten before they were read */
};

/*
 * Bring head up to date.  'end' is whether END has fired since the last
 * call, and 'amount' is AMOUNT read in the same pass: the position in the
 * current pass after a match, and the whole ring after END until the next
 * match.  If a match on the ring's last byte sets AMOUNT before its END
 * is seen, it reads as the start of the pass and would go backwards, so
 * head only moves forwards.
 */
static inline void uartrx_written(struct uartrx *r, bool end, uint32_t amount)
{
	uint32_t pos;

	if (end)
		r->wraps++;
	pos = (r->wraps << UARTRX_SHIFT) + (amount == UARTRX_SIZE ? 0 : amount);
	if ((int32_t)(pos - r->head) > 0)
		r->head = pos;
}

/*
 * Skip what may have been overwritten, and say whether anything was.
 *
 * The DMA overwrites what has waited a whole ring, and it can be a packet
 * past head without saying so; one more packet's room covers the time it
 * takes to read the rest.  The link's windows keep the data in flight under
 * a ring, so only a loop stalled long enough for acknowledgements to fill
 * the rest gets here: seconds, measured.  END is a flag, so a stall of more
 * than a ring also loses a wrap.  The position is then a ring short, which
 * is the same place in the ring, and what is read until it catches up is
 * two passes spliced together, for the decoder to drop.
 */
static inline bool uartrx_overrun(struct uartrx *r)
{
	if (r->head - r->tail <= UARTRX_SIZE - 2 * LINK_WIRE_MAX)
		return false;
	r->lost += r->head - r->tail;
	r->tail = r->head;
	return true;
}

#endif /* UARTRX_H */
