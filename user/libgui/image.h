#pragma once

#include <gui.h>

// Each format's decoder, which fills in image with pixels from malloc, or returns
// false having said why with image_fail.
bool png_decode(const uint8_t *data, size_t size, struct gui_image *image);
bool jpeg_decode(const uint8_t *data, size_t size, struct gui_image *image);
bool bmp_decode(const uint8_t *data, size_t size, struct gui_image *image);

bool image_fail(const char *why);

// Pixels for an image of a size, refusing ones too big to be real.
bool image_alloc(struct gui_image *image, long width, long height);
bool gif_decode(const uint8_t *data, size_t size, struct gui_image *image);
bool webp_decode(const uint8_t *data, size_t size, struct gui_image *image);
