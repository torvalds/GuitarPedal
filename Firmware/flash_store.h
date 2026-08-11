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
// alone want more than that.  The RP2354A has 2MB of flash on the die
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
// What a scan found.  Diagnostic rather than load-bearing: the pedal
// reports it in the identity reply so that slots planted with picotool
// can be asked about from a shell, without any of this having to grow a
// debug interface of its own - and in particular without the shipped
// firmware being able to be told to write flash.
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

//
// The staging buffer, which is also just the buffer.
//
// A caller does not build a payload somewhere and hand 4kB over - it
// asks for this, fills it in, and then commits it under a key.  One
// copy of a big buffer instead of two, and the awkward question of who
// owns the other one never comes up.
//
// It has to be in RAM and it has to be static.  In RAM because
// flash_range_program() reads its source with XIP switched off, so a
// payload living in flash would program garbage; static because core
// 0's stack is 2kB and this is 4.
//
static struct save_slot save_staging;

//
// Cleared on the way out, so that whatever the caller does not write is
// zero rather than the last save's contents.  Two reasons: the padding
// goes into the hash, and one of the ways this could leak something is
// by writing out a buffer somebody else filled.
//
static inline uint8_t *save_buffer(void)
{
	memset(&save_staging, 0, sizeof(save_staging));
	return save_staging.payload;
}

//
// Which slot the next write should go to, and what sequence it gets.
//
// The rule that matters: never a slot that is the only intact copy of
// its key.  Picking simply the oldest would let thirty-three saves of
// one scene quietly erase a different scene that had not been touched.
//
// So, in order of preference: a slot holding nothing - never written,
// or written and since found broken - and failing that the superseded
// copy with the lowest sequence.  If neither exists then every slot is
// somebody's only copy, and the honest answer is to refuse rather than
// to destroy one.
//
static inline bool save_next_write(unsigned *slot_out, uint32_t *seq_out)
{
	uint64_t good = 0, winner = 0;
	uint32_t newest = 0;
	bool any = false;

	for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
		const struct save_slot *slot = save_slot(i);

		if (!save_marked(slot) || !save_verify(slot))
			continue;
		good |= 1ull << i;
		if (!any || slot->tail.seq > newest)
			newest = slot->tail.seq;
		any = true;
	}

	//
	// A slot is the winner for its key unless some other valid slot
	// with the same key beats it.  Ties go to the lower index, which
	// is the same way save_read() breaks them - they must agree, or
	// a write would reclaim the slot a read is about to return.
	//
	for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
		if (!(good & (1ull << i)))
			continue;

		bool beaten = false;
		for (unsigned j = 0; j < SAVE_SLOT_COUNT && !beaten; j++) {
			if (j == i || !(good & (1ull << j)))
				continue;
			if (save_slot(j)->tail.key != save_slot(i)->tail.key)
				continue;
			beaten = save_slot(j)->tail.seq > save_slot(i)->tail.seq ||
				 (save_slot(j)->tail.seq == save_slot(i)->tail.seq &&
				  j < i);
		}
		if (!beaten)
			winner |= 1ull << i;
	}

	int best = -1;
	uint32_t best_seq = 0;

	for (unsigned i = 0; i < SAVE_SLOT_COUNT; i++) {
		if (!(good & (1ull << i))) {
			best = i;
			break;
		}
		if (winner & (1ull << i))
			continue;
		if (best < 0 || save_slot(i)->tail.seq < best_seq) {
			best = i;
			best_seq = save_slot(i)->tail.seq;
		}
	}

	if (best < 0)
		return false;

	//
	// Unreachable by wear - the flash dies after a few million
	// writes - but a planted slot can put any number here, and
	// SAVE_SEQ_NONE has to keep meaning 'erased'.
	//
	if (newest >= SAVE_SEQ_NONE - 1)
		return false;

	*slot_out = best;
	*seq_out = any ? newest + 1 : 1;
	return true;
}

//
// Write the staging buffer to flash under a key.
//
// Core 0 stops for the duration and core 1 keeps playing, which is what
// all the __not_in_flash("audio") marking is in aid of - see
// check-audio.py.  Interrupts go off because with XIP disabled there is
// nothing safe for a handler to run from; core 1 has none of its own.
//
// How long core 0 is gone for, measured on the pedal over ten writes:
//
//	erase	  30.3 - 37.3 ms
//	program	   8.7 -  8.8 ms
//	verify	   0.42 - 0.43 ms	(interrupts back on)
//
// So about 40ms, four fifths of it the erase, and comfortably short of
// the 300ms a datasheet will allow for a sector erase.  USB misses that
// many frames and the host may or may not mind; saving is a thing you
// do while setting the pedal up rather than while playing.
//
// If that ever becomes annoying, the erase is separable: erase the next
// free slot straight after a save rather than just before the following
// one, and what the person waiting sees is the 8.8ms program.  The
// format does not change for it.
//
static inline bool save_commit(uint16_t key)
{
	unsigned slot;
	uint32_t seq, offset;

	if (!save_next_write(&slot, &seq))
		return false;

	offset = SAVE_AREA_OFFSET + slot * SAVE_SLOT_SIZE;

	//
	// The one that matters.  Everything above says this cannot be
	// out of range, and the cost of being wrong is erasing the
	// firmware out from under the pedal - which takes a physical
	// BOOTSEL to recover from, on a pedal where the button is inside
	// the enclosure.  So check it anyway, against the region rather
	// than against the flash: flash_range_erase()'s own assert only
	// knows where the chip ends.
	//
	if (offset < SAVE_AREA_OFFSET ||
	    offset + SAVE_SLOT_SIZE > SAVE_AREA_OFFSET + SAVE_SLOT_COUNT * SAVE_SLOT_SIZE)
		return false;

	memcpy(save_staging.tail.marker, SAVE_MARKER,
	       sizeof(save_staging.tail.marker));
	save_staging.tail.seq = seq;
	save_staging.tail.version = SAVE_VERSION;
	save_staging.tail.key = key;
	memset(save_staging.tail.reserved, 0,
	       sizeof(save_staging.tail.reserved));

	{
		pico_sha256_state_t sha;
		sha256_result_t result;

		if (pico_sha256_try_start(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK)
			return false;
		pico_sha256_update(&sha, (const uint8_t *)&save_staging,
				   SAVE_HASHED_SIZE);
		pico_sha256_finish(&sha, &result);
		memcpy(save_staging.tail.hash, result.bytes,
		       sizeof(save_staging.tail.hash));
	}

	//
	// Saving breaks the USB audio stream, and is allowed to.
	//
	// Both of these hold interrupts off for their whole duration, and a
	// 4kB sector erase is tens of milliseconds on its own.  Nothing on
	// core 0 runs meanwhile: not tud_task(), not usb_audio_task(), so
	// the audio endpoint is not fed and whatever is recording through
	// the pedal gets a hole.  A Save Scene is two commits - the scene
	// and then the globals - so it is that twice.
	//
	// Known and accepted rather than missed.  Interrupts have to be off
	// for a flash write on this part, and the alternatives all amount to
	// doing the write somewhere it can be interrupted, which it cannot.
	//
	// What makes it tolerable is that the audio *core* is untouched.
	// check-audio enforces that core 1 makes no flash reads, so XIP
	// going away does not stall the DSP or the analog path - the guitar
	// still comes out of the jack. It is core 0 and USB that stop, and
	// saving a scene is something done while setting a pedal up rather
	// than while playing through it.
	//
	{
		uint32_t irq = save_and_disable_interrupts();
		flash_range_erase(offset, SAVE_SLOT_SIZE);
		restore_interrupts(irq);
	}
	{
		uint32_t irq = save_and_disable_interrupts();
		flash_range_program(offset, (const uint8_t *)&save_staging,
				    SAVE_SLOT_SIZE);
		restore_interrupts(irq);
	}

	//
	// Read it back through XIP and check it twice: that the bytes
	// are the ones we meant, and that the hash they carry is one a
	// future boot will accept.  The first catches a bad write, the
	// second catches us having computed the hash wrong - which the
	// first would happily agree with.
	//
	return !memcmp(save_slot(slot), &save_staging, SAVE_SLOT_SIZE) &&
	       save_verify(save_slot(slot));
}

#endif // FLASH_STORE_H
