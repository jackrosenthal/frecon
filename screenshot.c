#include <png.h>
#include <stdlib.h>
#include <string.h>

#include "screenshot.h"
#include "util.h"

/*
 * Colors of an indexed PNG, found with an open addressing hash table:
 *  slots - index + 1 into colors of the color hashed to the slot, or 0.
 *  colors, ncolors - the palette.
 */
#define PALETTE_MAX 256
#define PALETTE_SLOTS 512

struct palette {
	uint16_t slots[PALETTE_SLOTS];
	uint32_t colors[PALETTE_MAX];
	unsigned int ncolors;
};

/* Returns the index of color, adding it if needed, or -1 if it is full. */
static int palette_index(struct palette* pal, uint32_t color)
{
	unsigned int slot = (color * 2654435761u) >> 23;

	while (pal->slots[slot]) {
		if (pal->colors[pal->slots[slot] - 1] == color)
			return pal->slots[slot] - 1;
		slot = (slot + 1) % PALETTE_SLOTS;
	}

	if (pal->ncolors == PALETTE_MAX)
		return -1;
	pal->colors[pal->ncolors] = color;
	pal->slots[slot] = ++pal->ncolors;
	return pal->ncolors - 1;
}

static void screenshot_write(png_struct* png, png_byte* data, size_t len)
{
	screenshot_t* screenshot = png_get_io_ptr(png);
	uint8_t* grown;

	grown = realloc(screenshot->data, screenshot->len + len);
	if (!grown)
		png_error(png, "out of memory");
	memcpy(grown + screenshot->len, data, len);
	screenshot->data = grown;
	screenshot->len += len;
}

static void screenshot_flush(png_struct* png)
{
}

/*
 * Encode the pixels as an indexed PNG with the colors in pal, or as an RGB
 * PNG if pal is NULL.
 */
static bool screenshot_encode(screenshot_t* screenshot, const uint32_t* pixels,
			      int32_t width, int32_t height,
			      struct palette* pal)
{
	png_struct* png;
	png_info* info;
	png_byte* volatile row = NULL;
	volatile bool ok = false;

	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png)
		return false;
	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, NULL);
		return false;
	}

	if (setjmp(png_jmpbuf(png)))
		goto done;

	png_set_write_fn(png, screenshot, screenshot_write, screenshot_flush);

	/* Most of the gain is from zlib on flat colors, not from effort. */
	png_set_compression_level(png, 3);

	if (pal) {
		png_color colors[PALETTE_MAX];
		int depth = pal->ncolors <= 2 ? 1 :
			    pal->ncolors <= 4 ? 2 :
			    pal->ncolors <= 16 ? 4 : 8;

		for (unsigned int i = 0; i < pal->ncolors; i++) {
			colors[i].red = (pal->colors[i] >> 16) & 0xff;
			colors[i].green = (pal->colors[i] >> 8) & 0xff;
			colors[i].blue = pal->colors[i] & 0xff;
		}
		png_set_IHDR(png, info, width, height, depth,
			     PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
			     PNG_COMPRESSION_TYPE_DEFAULT,
			     PNG_FILTER_TYPE_DEFAULT);
		png_set_PLTE(png, info, colors, pal->ncolors);
		png_set_filter(png, 0, PNG_FILTER_NONE);
		png_write_info(png, info);
		/* Rows are given one index per byte. */
		png_set_packing(png);
	} else {
		png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB,
			     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
			     PNG_FILTER_TYPE_DEFAULT);
		png_set_filter(png, 0, PNG_FILTER_SUB);
		png_write_info(png, info);
	}

	row = malloc((size_t)width * 3);
	if (!row)
		goto done;

	for (int32_t y = 0; y < height; y++) {
		const uint32_t* src = &pixels[y * width];

		for (int32_t x = 0; x < width; x++) {
			if (pal) {
				row[x] = palette_index(pal, src[x] & 0xffffff);
			} else {
				row[x * 3 + 0] = (src[x] >> 16) & 0xff;
				row[x * 3 + 1] = (src[x] >> 8) & 0xff;
				row[x * 3 + 2] = src[x] & 0xff;
			}
		}
		png_write_row(png, row);
	}

	png_write_end(png, NULL);
	ok = true;

done:
	free(row);
	png_destroy_write_struct(&png, &info);
	return ok;
}

screenshot_t* screenshot_capture(fb_t* fb)
{
	int32_t width = fb_getwidth(fb);
	int32_t height = fb_getheight(fb);
	struct palette* pal;
	screenshot_t* screenshot;
	uint32_t* pixels;
	bool ok;

	pixels = fb_capture(fb);
	if (!pixels)
		return NULL;

	screenshot = calloc(1, sizeof(*screenshot));
	pal = calloc(1, sizeof(*pal));
	if (!screenshot || !pal) {
		free(pixels);
		free(screenshot);
		free(pal);
		return NULL;
	}

	/* Text usually has few enough colors for a palette. */
	for (size_t i = 0; i < (size_t)width * height; i++) {
		if (palette_index(pal, pixels[i] & 0xffffff) < 0) {
			free(pal);
			pal = NULL;
			break;
		}
	}

	ok = screenshot_encode(screenshot, pixels, width, height, pal);
	free(pixels);
	free(pal);
	if (!ok) {
		LOG(ERROR, "Failed to encode the screenshot.");
		screenshot_destroy(screenshot);
		return NULL;
	}

	return screenshot;
}

void screenshot_destroy(screenshot_t* screenshot)
{
	if (!screenshot)
		return;
	free(screenshot->data);
	free(screenshot);
}
