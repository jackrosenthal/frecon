#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "fb.h"

typedef struct {
	uint8_t* data;
	size_t len;
} screenshot_t;

/*
 * Capture the screen of fb, as it is shown, as a PNG. Returns NULL on
 * failure.
 */
screenshot_t* screenshot_capture(fb_t* fb);
void screenshot_destroy(screenshot_t* screenshot);

#endif
