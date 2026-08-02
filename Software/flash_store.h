//
// Save areas in internal flash.
//
// The pedal's persistent storage, and deliberately nothing to do with
// what gets stored in it.  What it offers is a handful of ~4kB slots,
// each tagged with a key the layer above chooses, and the promise that
// reading a key gives you back the newest copy that is intact.  The
// contents are somebody else's business.
//
// This exists because the external EEPROM ran out.  The oldest boards
// carry a 2kbit part - 256 bytes, one scene - and the control bindings
// alone want more than that.  The RP2354B has 2MB of flash on the die
// and the firmware occupies 84kB of it, so the answer was never going
// to be a bigger EEPROM.
//
//
// The geometry.
//
// A slot is one 4kB erase sector, because that is the smallest thing
// that can be erased and there is no reason to be cleverer with 2MB to
// spend.  Sixty-four of them sit at the top of flash - see
// SAVE_AREA_OFFSET in CMakeLists.txt, which is also where check-flash.py
// gets the number it guards the gap with.
//
// Sixty-four rather than the thirty-two that would hold everything once,
// because several slots are meant to be valid at the same time: one per
// scene is the expected shape.  A region with room for exactly one copy
// of everything cannot rotate at all, so it is the one size that does
// not work.
//
//
// What makes a slot valid.
//
// The tail carries a marker, a sequence number, a key and a SHA-256 of
// everything ahead of it.  The hash is the authority; the marker is only
// there so that a scan can skip an erased slot without hashing 4kB to
// find out it was erased.  Anything that trusts the marker alone has
// misunderstood it.
//
// Erased flash reads as 0xFF, so a slot that was never written fails the
// marker test.  A slot caught by a power cut between the erase and the
// program has no marker either.  One caught mid-program has a marker and
// a hash that does not match.  There is no torn state that reads as
// valid: the hash covers every byte before it, so either nothing moved
// and the old contents still verify, or something did and they do not.
//
// SHA-256 for a job a CRC would do, because the RP2350 has the hardware
// and pico_sha256 is right there.  It costs a few milliseconds of boot
// and buys the question never being asked again.
//
//
// Sequence numbers are not state.
//
// They exist to order two copies of the same key, and that is all.
// Nothing keeps one across a reboot and nothing keeps one between
// writes: a write rescans and works out the next number from what is
// actually on the flash.  Sixty-four sixteen-byte header reads is
// nothing next to an erase, and it means there is no cached idea of the
// world that can be wrong.
//
#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <string.h>
#include <stddef.h>
#include "hardware/flash.h"
#include "pico/sha256.h"

#if SAVE_SLOT_SIZE != FLASH_SECTOR_SIZE
#error "a save slot is one erase sector"
#endif

//
// The tail, at the end of every slot.  Byte offsets are part of the
// on-flash format: this is written by a pedal and read by the next
// firmware to be flashed onto it, so the layout is not free to drift.
//
#define SAVE_MARKER "SAVEAREA"	// eight bytes, and no NUL - see below
#define SAVE_VERSION 1

struct save_tail {
	char marker[8];			// SAVE_MARKER, not a C string
	uint32_t seq;			// higher wins, within one key
	uint16_t version;		// SAVE_VERSION
	uint16_t key;			// opaque here
	uint8_t reserved[16];		// written zero, ignored on read
	uint8_t hash[32];		// SHA-256 of everything above
};

//
// The tail is an on-flash layout, so spell the offsets out rather than
// trusting that nobody ever reorders a field or that the compiler packs
// it the way it looks.  tools/saveslot builds these images from the
// other side, in python, and cannot share this declaration - so this is
// the only thing keeping the two spellings of the format together.
//
_Static_assert(sizeof(struct save_tail) == 64, "save tail is 64 bytes");
_Static_assert(offsetof(struct save_tail, marker) == 0, "marker at 0");
_Static_assert(offsetof(struct save_tail, seq) == 8, "seq at 8");
_Static_assert(offsetof(struct save_tail, version) == 12, "version at 12");
_Static_assert(offsetof(struct save_tail, key) == 14, "key at 14");
_Static_assert(offsetof(struct save_tail, reserved) == 16, "reserved at 16");
_Static_assert(offsetof(struct save_tail, hash) == 32, "hash at 32");

#define SAVE_PAYLOAD_SIZE (SAVE_SLOT_SIZE - sizeof(struct save_tail))
#define SAVE_HASHED_SIZE (SAVE_SLOT_SIZE - sizeof(((struct save_tail *)0)->hash))

struct save_slot {
	uint8_t payload[SAVE_PAYLOAD_SIZE];
	struct save_tail tail;
};

_Static_assert(sizeof(struct save_slot) == SAVE_SLOT_SIZE, "slot is a sector");

//
// 0xFFFFFFFF is what an erased slot reads as, so it cannot also mean a
// real sequence number.  Nothing will ever get near it - the flash wears
// out after a few million writes - so losing the top value costs
// nothing.
//
#define SAVE_SEQ_NONE 0xFFFFFFFFu

//
// Slots are read straight out of the XIP window.  Reading is just
// pointer arithmetic; only writing has to care that flash is flash.
//
static inline const struct save_slot *save_slot(unsigned n)
{
	return (const struct save_slot *)(XIP_BASE + SAVE_AREA_OFFSET +
					  n * SAVE_SLOT_SIZE);
}

//
// Does this slot carry a marker at all?  Cheap, and wrong on its own -
// it says "worth hashing", never "good".
//
static bool save_marked(const struct save_slot *slot)
{
	return !memcmp(slot->tail.marker, SAVE_MARKER, sizeof(slot->tail.marker)) &&
	       slot->tail.version == SAVE_VERSION &&
	       slot->tail.seq != SAVE_SEQ_NONE;
}

//
// The real test.  Hashes the slot and compares.
//
// Takes the hardware unit for the duration; pico_sha256_try_start() will
// refuse if something else has it, which cannot happen today - there is
// one caller - but is worth failing closed on rather than ignoring.  A
// slot we cannot hash is a slot we cannot vouch for.
//
static bool save_verify(const struct save_slot *slot)
{
	pico_sha256_state_t sha;
	sha256_result_t result;

	if (pico_sha256_try_start(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK)
		return false;

	pico_sha256_update(&sha, (const uint8_t *)slot, SAVE_HASHED_SIZE);
	pico_sha256_finish(&sha, &result);

	return !memcmp(result.bytes, slot->tail.hash, sizeof(result.bytes));
}

//
// What the boot scan found.  Diagnostic rather than load-bearing: the
// pedal reports it in the identity reply so that a test can plant slots
// with picotool and ask what happened to them, without any of this
// having to grow a debug interface of its own.
//
struct save_scan {
	uint8_t marked;			// slots carrying a marker
	uint8_t valid;			// ...of which these also hashed
	uint8_t keys;			// distinct keys among the valid
	uint32_t newest;		// highest sequence seen, valid only
};

//
// Read every slot once and say what is there.
//
// Hashes every marked slot rather than stopping early, because the
// point of it is the report: "one of these two has a bad signature" is
// the answer being looked for, and you only get that by checking both.
//
_Static_assert(SAVE_SLOT_COUNT <= 64, "the valid-slot bitmap is 64 bits");

static void save_scan(struct save_scan *out)
{
	uint64_t good = 0;

	memset(out, 0, sizeof(*out));

	//
	// Hash each marked slot exactly once and remember which passed.
	// The bitmap is the whole reason for two passes: counting keys
	// by asking "was an earlier slot valid and the same key" reads
	// far better as a nested loop, but written that way it verifies
	// the same slot over and over - sixty-four slots would hash
	// eight megabytes to answer a question about sixty-four keys.
	//
	for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
		const struct save_slot *slot = save_slot(i);

		if (!save_marked(slot))
			continue;
		out->marked++;
		if (!save_verify(slot))
			continue;

		good |= 1ull << i;
		out->valid++;
		if (out->valid == 1 || slot->tail.seq > out->newest)
			out->newest = slot->tail.seq;
	}

	for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
		if (!(good & (1ull << i)))
			continue;

		bool seen = false;
		for (unsigned j = 0; j < i && !seen; j++)
			seen = (good & (1ull << j)) &&
			       save_slot(j)->tail.key == save_slot(i)->tail.key;
		if (!seen)
			out->keys++;
	}
}

//
// The newest intact copy of one key, or NULL.
//
// Ordered by sequence and verified from the top down, stopping at the
// first that passes - so the usual case hashes exactly one slot, and a
// slot that was being written when the power went hands over to the
// copy underneath it rather than to nothing.
//
static inline const void *save_read(uint16_t key)
{
	//
	// Only sequences below this are still candidates.  A slot that
	// fails the hash lowers it to that slot's own sequence, which
	// drops the failure and everything level with it and leaves the
	// copy underneath - so a write caught by a power cut hands over
	// to its predecessor rather than to nothing.
	//
	// A ceiling rather than a set of rejected slots because nothing
	// here should need memory: save_marked() already refuses
	// SAVE_SEQ_NONE, so every real sequence starts out below it.
	//
	uint32_t ceiling = SAVE_SEQ_NONE;

	for (;;) {
		const struct save_slot *best = NULL;

		for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
			const struct save_slot *slot = save_slot(i);

			if (!save_marked(slot) || slot->tail.key != key)
				continue;
			if (slot->tail.seq >= ceiling)
				continue;
			if (best && slot->tail.seq <= best->tail.seq)
				continue;
			best = slot;
		}

		if (!best)
			return NULL;
		if (save_verify(best))
			return best->payload;

		ceiling = best->tail.seq;
	}
}

#endif // FLASH_STORE_H
