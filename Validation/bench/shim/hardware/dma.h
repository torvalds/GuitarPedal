//
// A DMA controller made of ordinary memory.
//
// audio/effect.h reads exactly two fields - the read address of the
// transmit channel and the write address of the receive channel - and
// turns each into a pointer into i2s_dma_buf[].  That is the whole of
// the audio core's contact with the hardware, so a struct with those two
// fields in it is a complete substitute, and single_sample() runs
// unmodified against it.
//
// The bench moves these two "registers" between samples the way the DMA
// engine would: rx one slot ahead of the cpu, so the spin exits at once,
// and tx well behind it, so the deadline check stays false.  See
// bench.c.
//
// uintptr_t rather than the SDK's uint32_t.  The firmware stores a
// 32-bit address in a 32-bit register; here the addresses are the host's
// and a uint32_t would truncate every one of them.  The '& ~7' in
// effect.h still does the right thing, because ~7 widens to all-ones
// with the low three bits clear.
//
#ifndef _BENCH_HARDWARE_DMA_H
#define _BENCH_HARDWARE_DMA_H

#include <stdint.h>

#define BENCH_DMA_CHANNELS 16

struct bench_dma_channel {
	uintptr_t read_addr;
	uintptr_t write_addr;
	uint32_t transfer_count;
	uint32_t ctrl_trig;
};

struct bench_dma {
	struct bench_dma_channel ch[BENCH_DMA_CHANNELS];
};

extern struct bench_dma bench_dma_regs;

#define dma_hw (&bench_dma_regs)

#endif
