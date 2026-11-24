// WebP, decoded by libwebp's decoder in its own library, loaded the first time
// one's needed, so programs that never show one don't carry it.
#include "image.h"
#include <dlfcn.h>
#include <stdlib.h>

#ifndef LIBWEBP // which Linux builds name as their own
#define LIBWEBP "libwebp.so.7" // Arch's
#endif

bool webp_decode(const uint8_t *data, size_t size, struct gui_image *image) {
    static uint8_t *(*decode)(const uint8_t *, size_t, int *, int *); // WebPDecodeBGRA
    int width, height;
    uint8_t *pixels;

    if (!decode) {
        void *library = dlopen(LIBWEBP, RTLD_NOW);

        if (!library || !(decode = (uint8_t *(*)(const uint8_t *, size_t, int *, int *))dlsym(library, "WebPDecodeBGRA")))
            return image_fail("can't load " LIBWEBP);
    }
    // Blue, green, red and alpha bytes are 0xAARRGGBB, and libwebp's memory is malloc's.
    if (!(pixels = decode(data, size, &width, &height)))
        return image_fail("a broken WebP");
    *image = (struct gui_image){(uint32_t *)pixels, width, height};
    return true;
}
