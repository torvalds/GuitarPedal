//
// Some basic initialization and drawing primitives for
// the SH1106 / SSD1306 128x64 monochrome OLED drivers.
//
//   https://cdn.velleman.eu/downloads/29/infosheets/sh1106_datasheet.pdf
//
// See SH1107 for the 128x128 pixel version:
//
//   https://www.displayfuture.com/Display/datasheet/controller/SH1107.pdf
//
// However, while my module was sold as a SH1106, it seems to be a
// SSD1306 since it requires that "0x8D 0x14" command sequence to turn
// the charge pump on:
//
//   https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
//
#define SH1106_ADDR	0x3C

#define SH1107 1

// Direction of display scanning
#define SH1106_LEFTRIGHT	0	// 0 - horizongal scan direction
#define SH1106_UPDOWN		0	// 0 - vertical scan direction

// The SH1106 controller does 132 columns, might be offset
// One of the random differences to the SH1306
#define SH1106_HOFFSET		0

#ifdef SH1107
#define SH110x_PAGES 16
#else
#define SH110x_PAGES 8
#endif

#define SH110x_COLUMNS 128
#define SH110x_LINES (SH110x_PAGES*8)
#define SH110x_PAGEMASK (SH110x_PAGES-1)

static int sh1106_read(void)
{
	unsigned char rxdata;
	return i2c_read_blocking(SH1106_I2C, &rxdata, 1, false);
}

static int __sh1106_array_write(const unsigned char *data, size_t len)
{
	return i2c_write_blocking(SH1106_I2C, data, len, false);
}
#define sh1106_array_write(arr) __sh1106_array_write(arr, ARRAY_SIZE(arr))

struct sh1106_page {
	unsigned char dirt_start, dirt_end;
	char data[132];
};
static struct sh1106_page framebuffer[SH110x_PAGES] = {
	[0 ... SH110x_PAGES-1] = { 0, 128, },
};

static inline void sh1106_sprite_column(int pageidx, int posX, char clr, char xor)
{
	if (pageidx & ~SH110x_PAGEMASK)
		return;
	if (!(clr || xor))
		return;
	struct sh1106_page *page = framebuffer + pageidx;
	unsigned char old = page->data[posX];
	unsigned char new = (old & ~clr) ^ xor;
	if (old == new)
		return;
	page->data[posX] = new;
	if (posX < page->dirt_start)
		page->dirt_start = posX;
	if (posX >= page->dirt_end)
		page->dirt_end = posX+1;
}

static inline unsigned int rot32(unsigned int val, int shift)
{
	return (val << shift) | (val >> (32-shift));
}

static inline void sh1106_fill(int x, int y, int w, int h, unsigned int val, int shift, unsigned int clr)
{
	if (x >= SH110x_COLUMNS || y >= SH110x_LINES) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (w <= 0 || h <= 0) return;
	w += x;
	if (w > 128) w = 128;
	shift &= 31;

	int page = y >> 3;
	y &= 7; h += y;
	unsigned int bits = ~0 << y;

	do {
		unsigned int pattern = val;
		if (h < 8)
			bits &= (1 << h)-1;
		for (int i = x; i < w; i++) {
			sh1106_sprite_column(page, i, clr & bits, pattern & bits);
			pattern = rot32(pattern, shift);
		}

		bits = ~0;
		page++;
		h -= 8;
	} while (h > 0 && page < SH110x_PAGES);
}

#define sh1106_clear(x,y,w,h) sh1106_fill(x,y,w,h,0,0,~0u)
#define sh1106_reverse(x,y,w,h) sh1106_fill(x,y,w,h,~0u,0,0)

static inline void sh1106_hline(int x, int y, int w)
{
	if (x >= SH110x_COLUMNS || y >= SH110x_LINES) return;
	if (x < 0) { w += x; x = 0; }
	if (w <= 0 || y < 0) return;
	w += x;
	if (w > 128) w = 128;

	int page = y >> 3;
	unsigned int mask = 1 << (y & 7);
	for (int i = x; i < w; i++)
		sh1106_sprite_column(page, i, mask, mask);
}

static inline void sh1106_vline(int x, int y, int h)
{
	if (x >= SH110x_COLUMNS || y >= SH110x_LINES) return;
	if (y < 0) { h += y; y = 0; }
	if (h <= 0 || x < 0) return;

	int page = y >> 3;
	y &= 7; h += y;
	unsigned int clr = ~0 << y;

	do {
		if (h < 8)
			clr &= (1 << h)-1;
		sh1106_sprite_column(page, x, clr, clr);
		clr = ~0;
		page++;
		h -= 8;
	} while (h > 0 && page < SH110x_PAGES);
}

static void sh1106_sprite(int posX, int posY, int width, const unsigned *clrp, const unsigned *xorp)
{
	if (posY >= SH110x_LINES || posX >= SH110x_COLUMNS)
		return;
	for (int i = 0; i < width; i++, posX++) {
		unsigned clr = *clrp++, xor = *xorp++;
		int page;

		if (posX < 0)
			continue;
		if (!(clr || xor))
			continue;
		if (posX >= SH110x_COLUMNS)
			break;
		clr <<= posY & 7;
		xor <<= posY & 7;
		page = posY >> 3;
		do {
			sh1106_sprite_column(page, posX, clr, xor);
			page++; clr >>= 8; xor >>= 8;
		} while (clr || xor);
	}
}

//
// Fill patterns for sh1106_rectangle().
// Note that all patterns except 'rect_filled' will explicitly draw a solid 1-pixel
// border around the outside of the rectangle. To clear an area without leaving an
// unexpected outline, use sh1106_clear() or sh1106_fill() directly.
//
enum pattern {
	rect_border,        // Solid outline, interior untouched
	rect_filled,        // Solid white box (no discrete border logic)
	rect_clear,         // Solid outline, interior filled with 0s (black)
	rect_stipple,       // Solid outline, interior 50% dot stipple
	rect_lines,         // Solid outline, interior horizontal lines
};

static void sh1106_rectangle(int posX, int posY, int width, int height, enum pattern fill)
{
	if (fill == rect_filled)
		return sh1106_fill(posX, posY, width, height, ~0, 0, ~0);

	if (width < 1 || height < 1)
		return;

	sh1106_hline(posX, posY, width);
	sh1106_hline(posX, posY+height-1, width);
	sh1106_vline(posX, posY, height);
	sh1106_vline(posX+width-1, posY, height);

	if (fill == rect_border)
		return;
	if (width < 2 || height < 2)
		return;

	posX++; posY++; width-=2; height-=2;
	static const unsigned int fill_pattern[] = {
		[rect_stipple] = 0x55555555,
		[rect_lines] = 0x11111111,
		[rect_clear] = 0
	};
	unsigned int pattern = fill_pattern[fill];
	sh1106_fill(posX, posY, width, height, pattern, 1, ~0);
}

#include "font_6x8.h"
#include "font_8x16.h"

static struct {
	unsigned int mask[6], bitmap[6];
} glyph_6x8 = { .mask = { [0 ... 5] = 0xff } };

static struct {
	unsigned int mask[8], bitmap[8];
} glyph_8x16 = { .mask = { [0 ... 7] = 0xffff } };

#define __RENDER(x,y,font,glyph,c) do {				\
	const unsigned width = ARRAY_SIZE(glyph.bitmap);	\
	__auto_type fontdata = font+c*width;   			\
	for (int i = 0; i < width; i++)				\
		glyph.bitmap[i] = fontdata[i];			\
	sh1106_sprite(x, y, width, glyph.mask, glyph.bitmap);	\
} while (0)

#define RENDER(name,x,y,c) \
	__RENDER(x,y,font_##name,glyph_##name,(unsigned char)(c))

static void sh1106_puts_6x8(int x, int y, const char *s)
{
	for (char c; (c = *s) != 0; s++) {
		RENDER(6x8,x,y,c);
		x += 6;
	}
}

static void sh1106_puts_8x16(int x, int y, const char *s)
{
	for (char c; (c = *s) != 0; s++) {
		RENDER(8x16,x,y,c);
		x += 8;
	}
}

static void sh1106_graph_vline(int start, int last, int y, int min, int max)
{
	if (y < min) y = min;
	if (y > max) y = max;
	if (last < min) last = min;
	if (last > max) last = max;

	int mid = (y+last)/2;
	if (last <= y) {
		sh1106_vline(start-1, last, mid-last);
		sh1106_vline(start, mid, y-mid+1);
	} else {
		sh1106_vline(start-1, mid, last-mid);
		sh1106_vline(start, y, mid-y+1);
	}
}

static void sh1106_graph(int start, int end, int min, int max, int (*fn)(int, void*), void *arg)
{
	if (min > max)
		return;
	if (start < 0)
		start = 0;
	if (end > 127)
		end = 127;
	if (start > end)
		return;

	int last = fn(start, arg);
	if (last < min)
		last = min-1;
	else if (last > max)
		last = max+1;
	else
		sh1106_vline(start, last, 1);

	while (++start <= end) {
		int y = fn(start, arg);
		if (y < min)
			y = min-1;
		if (y > max)
			y = max+1;
		if (last < min) {
			if (y < min)
				continue;
		} else if (last > max) {
			if (y > max)
				continue;
		}
		sh1106_graph_vline(start, last, y, min, max);
		last = y;
	}
}

static inline void sh1106_init(void)
{
	// See also "Software Configuration" in Solomon Systech app note
	static const char initcmd[] = {
		0xAE,				// Display OFF
		0x00,				// Lower column address
		0x10,				// Higher column address zero (default)
		0x20,				// Page addressing mode (SH1107)
		0x32,				// Charge pump at 8V (default)
		0x40,				// Set display start line zero (default)
		0x81, 0x80,			// Contrast control
		0x8D, 0x14,			// Chargepump enable - see SSD1306 app note
		0xA0 + SH1106_LEFTRIGHT,	// Set segment remap left rotate
		0xA4,				// Set scan from RAM on (default)
		0xA6,				// Set normal display (default)
		0xA8, SH110x_LINES-1,		// Set multiplex ratio to 64/128 (default)
		0xAD, 0x8B,			// Set DC-DC ON (display must be off)
		0xB0,				// Page address 0
		0xC0 +8*SH1106_UPDOWN,		// Common output scan direction: normal
		0xD3, 0x00,			// Set display offset (0-3f)
		0xD5, 0x80,			// Display clock divide ratio: f_osc (upper nybble) divided by 1 (lower nybble) (default)
		0xD9, 0xF1,			// Set dischange/precharge period, 15 / 1 DCLK each
		0xDA, 0x12,			// Set pad configuration sequential
		0xDB, 0x35,			// Set VCOM deselect level (0.770 - default)
		// 0xE0: R-M-W command - reads do not increment addresses, writes do
		// 0xE3: NOP
		// 0xEE: End R-M-W mode
		0xAF,		// Display ON
	};

	for (int i = 0; i < ARRAY_SIZE(initcmd); i++) {
		unsigned char cmd[2] = { 0x80, initcmd[i] };
		sh1106_array_write(cmd);
	}
}

static int sh1106_dirty;

static void sh1106_draw(void)
{
	sh1106_dirty = 1;
}

static void sh1106_task()
{
	static int dma_chan = -1;
	static uint16_t dma_buffer[150];

	if (dma_chan == -1) {
		const struct {
			i2c_inst_t *i2c;
			unsigned char addr;
		} sh1106 = { SH1106_I2C };
		i2c_hw_t *i2c_hw = i2c_get_hw(sh1106.i2c);

		dma_chan = dma_claim_unused_channel(true);
		dma_channel_config c = dma_channel_get_default_config(dma_chan);

		channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
		channel_config_set_dreq(&c, i2c_get_dreq(sh1106.i2c, true));

		dma_channel_configure(dma_chan, &c,
			&i2c_hw->data_cmd,
			dma_buffer,
			0, // transfer count set per transfer
			false);

		// Manually setup I2C target address before transfer
		i2c_hw->enable = 0;
		i2c_hw->tar = sh1106.addr;
		i2c_hw->enable = 1;
	}

	if (dma_channel_is_busy(dma_chan))
		return;

	if (!sh1106_dirty)
		return;

	static int init = 0;
	if (!init) {
		if (sh1106_read() < 0)
			return;
		init = 1;
		sh1106_init();
		return;
	}

	size_t ptr = 0;

	for (int i = 0; i < SH110x_PAGES; i++) {
		struct sh1106_page *page = framebuffer+i;
		unsigned char start = page->dirt_start;
		unsigned char end = page->dirt_end;
		unsigned char column;

		if (start >= end)
			continue;

		column = start + SH1106_HOFFSET;
		if (column > 127) {
			column = start;
			start -= SH1106_HOFFSET;
		}

		// Set page
		dma_buffer[ptr++] = 0x80;
		dma_buffer[ptr++] = 0xB0 + i;

		// Set column
		dma_buffer[ptr++] = 0x80;
		dma_buffer[ptr++] = 0x00 + (column & 0xf);
		dma_buffer[ptr++] = 0x80;
		dma_buffer[ptr++] = 0x10 + (column >> 4);

		// Actual frame data
		dma_buffer[ptr++] = 0x40; // Data follows

		size_t bytes = end - start;
		for (size_t j = 0; j < bytes; j++) {
			dma_buffer[ptr++] = page->data[start + j];
		}

		// Set STOP bit on the last byte of this page's chunk
		// so the SH1106 knows the data stream ended. The I2C hardware
		// will automatically generate a new START + Address for the next chunk.
		dma_buffer[ptr - 1] |= (1 << 9);

		// Reset dirty bounds to their opposite maximums
		page->dirt_start = 255;
		page->dirt_end = 0;

		dma_channel_transfer_from_buffer_now(dma_chan, dma_buffer, ptr);
		return;
	}

	sh1106_dirty = 0;
}
