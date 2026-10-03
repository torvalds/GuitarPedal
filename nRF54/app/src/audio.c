/*
 * The i2s link with the RP2354, which for now carries only the test
 * pattern in i2stest.h, both ways: received as a slave - the RP2354 drives
 * both clocks - and checked bit for bit, with a report once a second on the
 * debug stream, and sent back for the pedal to check the same way.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_i2s.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>

#include "audio.h"
#include "i2stest.h"

#define RATE		48000
#define BLOCK_FRAMES	256			/* 5.3 ms */
#define BLOCK_BYTES	(BLOCK_FRAMES * 2 * sizeof(int32_t))
#define BLOCKS		4

K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_BYTES, BLOCKS, 4);
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_BYTES, BLOCKS, 4);

/* A block is a whole number of the pattern, so one block repeats */
BUILD_ASSERT(BLOCK_FRAMES % I2STEST_FRAMES == 0);
static int32_t pattern[BLOCK_FRAMES * 2];

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s20));

static struct {
	bool running;
	struct i2stest_rx rx;	/* since the last report */
	uint32_t restarts;	/* nothing for a while: restarted */
	uint32_t block_at;	/* when a block last arrived */
	uint32_t reported_at;
} t;

/*
 * The pedal's frame rate, by this side's 32 MHz crystal.
 *
 * The i2s peripheral takes its next buffer at every block boundary, which
 * is exactly BLOCK_FRAMES of the pedal's frames, and that event is wired in
 * hardware to two timers: one captures a 16 MHz count, the other counts the
 * boundaries.  So the time of a boundary is known to a tick, with none of
 * the loop's lateness in it.  The high-frequency clock runs from the
 * crystal only while something asks for it, so this asks.
 */
#define MEASURE_CLOCK	0	/* a measurement: it holds the crystal on */

static NRF_TIMER_Type *const stamp = NRF_TIMER23;
static NRF_TIMER_Type *const bounds = NRF_TIMER24;
static struct onoff_client hfxo;

static struct {
	bool on, started;
	uint32_t n0;		/* boundaries at the first reading */
	uint32_t s_last;	/* the last capture, to unwrap the next */
	uint64_t ticks;		/* from the first reading's boundary */
	uint32_t n;		/* boundaries at the last reading */
} clk;

static void clock_start(void)
{
	nrfx_gppi_handle_t h;
	int err;

	sys_notify_init_spinwait(&hfxo.notify);
	err = onoff_request(z_nrf_clock_control_get_onoff(
				CLOCK_CONTROL_NRF_SUBSYS_HF), &hfxo);
	if (err < 0)
		printk("i2s: no crystal, %d\n", err);

	nrf_timer_mode_set(stamp, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(stamp, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(stamp, 0);
	nrf_timer_mode_set(bounds, NRF_TIMER_MODE_COUNTER);
	nrf_timer_bit_width_set(bounds, NRF_TIMER_BIT_WIDTH_32);

	err = nrfx_gppi_conn_alloc(
		nrf_i2s_event_address_get(NRF_I2S20, NRF_I2S_EVENT_RXPTRUPD),
		nrf_timer_task_address_get(stamp, NRF_TIMER_TASK_CAPTURE0), &h);
	if (!err)
		err = nrfx_gppi_ep_attach(
			nrf_timer_task_address_get(bounds, NRF_TIMER_TASK_COUNT),
			h);
	if (err) {
		printk("i2s: no clock measurement, %d\n", err);
		return;
	}
	nrfx_gppi_conn_enable(h);
	nrf_timer_task_trigger(stamp, NRF_TIMER_TASK_START);
	nrf_timer_task_trigger(bounds, NRF_TIMER_TASK_START);
	clk.on = true;
}

/* The boundaries so far and when the last began, both of the same one */
static void clock_read(uint32_t *n, uint32_t *s)
{
	uint32_t again;

	do {
		nrf_timer_task_trigger(bounds, NRF_TIMER_TASK_CAPTURE0);
		*n = nrf_timer_cc_get(bounds, NRF_TIMER_CC_CHANNEL0);
		*s = nrf_timer_cc_get(stamp, NRF_TIMER_CC_CHANNEL0);
		nrf_timer_task_trigger(bounds, NRF_TIMER_TASK_CAPTURE0);
		again = nrf_timer_cc_get(bounds, NRF_TIMER_CC_CHANNEL0);
	} while (again != *n);
}

/*
 * Brought up to date once a second, which also unwraps the capture: it
 * wraps every 268 s at 16 MHz.  The answer is how far from 48 kHz the
 * pedal's frames are by this side's crystal, in parts per billion, and
 * the seconds it is averaged over.
 */
static bool clock_ppb(int64_t *ppb, uint32_t *secs)
{
	uint32_t n, s, f = NRF_TIMER_BASE_FREQUENCY_GET(stamp) >>
			     nrf_timer_prescaler_get(stamp);
	uint64_t frames;
	int64_t diff;

	if (!clk.on)
		return false;
	clock_read(&n, &s);
	if (!clk.started) {
		if (!n)
			return false;
		clk.n0 = clk.n = n;
		clk.s_last = s;
		clk.ticks = 0;
		clk.started = true;
		return false;
	}
	clk.ticks += (uint32_t)(s - clk.s_last);
	clk.s_last = s;
	clk.n = n;
	if (!clk.ticks)
		return false;

	frames = (uint64_t)(n - clk.n0) * BLOCK_FRAMES;
	diff = (int64_t)(frames * f) - (int64_t)(clk.ticks * RATE);
	*ppb = diff * 1000000 / (int64_t)(clk.ticks * RATE / 1000);
	*secs = clk.ticks / f;
	return true;
}

/* Keep the transmit queue full of the pattern; false if nothing went */
static bool feed(void)
{
	bool fed = false;
	void *block;

	while (k_mem_slab_alloc(&tx_slab, &block, K_NO_WAIT) == 0) {
		memcpy(block, pattern, BLOCK_BYTES);
		if (i2s_write(i2s, block, BLOCK_BYTES)) {
			k_mem_slab_free(&tx_slab, block);
			break;
		}
		fed = true;
	}
	return fed;
}

static void start(void)
{
	struct i2s_config cfg = {
		.word_size = 32,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
		.frame_clk_freq = RATE,
		.mem_slab = &rx_slab,
		.block_size = BLOCK_BYTES,
		.timeout = 0,
	};
	int err;

	err = i2s_configure(i2s, I2S_DIR_RX, &cfg);
	if (!err) {
		cfg.mem_slab = &tx_slab;
		err = i2s_configure(i2s, I2S_DIR_TX, &cfg);
	}
	if (!err) {
		feed();
		err = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	}
	if (err) {
		printk("i2s: does not start, %d\n", err);
		return;
	}
	t.running = true;
	t.block_at = k_uptime_get_32();
}

void audio_start(void)
{
	if (!device_is_ready(i2s)) {
		printk("i2s: not ready\n");
		return;
	}
	for (int i = 0; i < BLOCK_FRAMES; i++) {
		pattern[2 * i] = i2stest_left[i & I2STEST_MASK];
		pattern[2 * i + 1] = i2stest_right(i);
	}
	i2stest_rx_init(&t.rx);
	t.reported_at = k_uptime_get_32();
	start();
	if (MEASURE_CLOCK)
		clock_start();
}

void audio_poll(void)
{
	void *block;
	size_t size;
	uint32_t now;

	if (!t.running)
		return;

	/*
	 * With no timeout, an empty queue is -EIO just as an error is, so
	 * the return says nothing.  What says the driver has stopped - an
	 * overrun does that - is no block for many blocks' time.
	 */
	while (i2s_read(i2s, &block, &size) == 0) {
		const int32_t *s = block;

		for (size_t i = 0; i + 1 < size / sizeof(int32_t); i += 2)
			i2stest_check(&t.rx, s[i], s[i + 1]);
		k_mem_slab_free(&rx_slab, block);
		t.block_at = k_uptime_get_32();
	}

	feed();

	now = k_uptime_get_32();
	if (now - t.block_at > 100) {
		t.restarts++;
		t.rx.phase = -1;
		clk.started = false;	/* the event count starts again */
		i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
		start();
	}

	uint32_t ms = now - t.reported_at;

	if (ms >= 1000) {
		printk("i2s: %u frames/s, %s, %u wrong (%u swapped, "
		       "%u shifted), %u slips, %u restarts\n",
		       (uint32_t)((uint64_t)t.rx.frames * 1000 / ms),
		       t.rx.phase >= 0 ? "in step" : "not in step",
		       t.rx.wrong, t.rx.swapped, t.rx.shifted, t.rx.slips,
		       t.restarts);

		int64_t ppb;
		uint32_t secs;

		if (clock_ppb(&ppb, &secs)) {
			int64_t a = ppb < 0 ? -ppb : ppb;

			printk("i2s: %s%lld.%03lld ppm from 48 kHz by this "
			       "crystal, over %u s\n", ppb < 0 ? "-" : "+",
			       a / 1000, a % 1000, secs);
		}
		t.rx.frames = t.rx.wrong = t.rx.swapped = t.rx.shifted = 0;
		t.rx.slips = t.restarts = 0;
		t.reported_at = now;
	}
}
