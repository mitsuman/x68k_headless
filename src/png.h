#ifndef X68K_PNG_H
#define X68K_PNG_H

#include <stdint.h>

/* Writes an 8-bit RGB PNG.  Returns 0 on success, -1 on error. */
int png_write_rgb(const char *path, const uint8_t *rgb, unsigned width, unsigned height);

#endif
