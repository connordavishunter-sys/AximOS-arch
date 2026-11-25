#include "image.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DIMENSION 16384
#define MAX_FILE (64 * 1024 * 1024)

static const char *error = "";

bool image_fail(const char *why) {
    error = why;
    return false;
}

const char *gui_image_error(void) {
    return error;
}

bool image_alloc(struct gui_image *image, long width, long height) {
    if (width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
        return image_fail("an image too big, or of no size");
    image->pixels = malloc((size_t)width * height * sizeof(uint32_t));
    image->width = (int)width;
    image->height = (int)height;
    return image->pixels || image_fail("out of memory");
}

void gui_image_free(struct gui_image *image) {
    free(image->pixels);
    *image = (struct gui_image){0};
}

// Which format it is, from how it starts.
bool gui_image_decode(const void *data, size_t size, struct gui_image *image) {
    const uint8_t *d = data;

    *image = (struct gui_image){0};
    if (size >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return png_decode(d, size, image);
    if (size >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return jpeg_decode(d, size, image);
    if (size >= 2 && d[0] == 'B' && d[1] == 'M')
        return bmp_decode(d, size, image);
    if (size >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
        return gif_decode(d, size, image);
    if (size >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return webp_decode(d, size, image);
    return image_fail("not a PNG, JPEG, BMP, GIF or WebP");
}

bool gui_image_load(const char *path, struct gui_image *image) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    uint8_t *data = NULL;
    size_t done = 0;
    bool ok = false;

    *image = (struct gui_image){0};
    if (fd < 0 || fstat(fd, &st) < 0)
        image_fail("can't open it");
    else if (st.st_size <= 0 || st.st_size > MAX_FILE)
        image_fail(st.st_size > 0 ? "too big a file" : "an empty file");
    else if (!(data = malloc(st.st_size)))
        image_fail("out of memory");
    while (data && done < (size_t)st.st_size) {
        ssize_t n = read(fd, data + done, st.st_size - done);

        if (n <= 0)
            break;
        done += n;
    }
    if (data && done == (size_t)st.st_size)
        ok = gui_image_decode(data, done, image);
    else if (data)
        image_fail("can't read it");
    free(data);
    if (fd >= 0)
        close(fd);
    return ok;
}

// Half the size, each pixel the average of four.
static bool halve(const struct gui_image *from, struct gui_image *to) {
    if (!image_alloc(to, from->width / 2 ? from->width / 2 : 1, from->height / 2 ? from->height / 2 : 1))
        return false;
    for (int y = 0; y < to->height; y++) {
        for (int x = 0; x < to->width; x++) {
            int x0 = 2 * x < from->width ? 2 * x : from->width - 1, y0 = 2 * y < from->height ? 2 * y : from->height - 1;
            int x1 = x0 + 1 < from->width ? x0 + 1 : x0, y1 = y0 + 1 < from->height ? y0 + 1 : y0;
            uint32_t p[4] = {from->pixels[y0 * from->width + x0], from->pixels[y0 * from->width + x1],
                             from->pixels[y1 * from->width + x0], from->pixels[y1 * from->width + x1]};
            uint32_t out = 0;

            for (int shift = 0; shift < 32; shift += 8)
                out |= (uint32_t)(((p[0] >> shift & 0xFF) + (p[1] >> shift & 0xFF) + (p[2] >> shift & 0xFF)
                                   + (p[3] >> shift & 0xFF) + 2) / 4) << shift;
            to->pixels[y * to->width + x] = out;
        }
    }
    return true;
}

// Halving while it's more than twice too big keeps fine detail from turning to
// noise, and then each pixel blends the four nearest.
bool gui_image_scale(const struct gui_image *from, int width, int height, struct gui_image *to) {
    struct gui_image smaller = {0};
    const struct gui_image *source = from;

    *to = (struct gui_image){0};
    while (source->width >= 2 * width && source->height >= 2 * height) {
        struct gui_image half;

        if (!halve(source, &half)) {
            gui_image_free(&smaller);
            return false;
        }
        gui_image_free(&smaller);
        smaller = half;
        source = &smaller;
    }
    if (!image_alloc(to, width, height)) {
        gui_image_free(&smaller);
        return false;
    }
    for (int y = 0; y < height; y++) {
        // Where the middle of this pixel falls in the source, in 1/256ths.
        long sy = ((2L * y + 1) * source->height * 128) / height - 128;
        int y0 = sy < 0 ? 0 : (int)(sy >> 8), fy = sy < 0 ? 0 : (int)(sy & 255);
        int y1 = y0 + 1 < source->height ? y0 + 1 : y0;

        for (int x = 0; x < width; x++) {
            long sx = ((2L * x + 1) * source->width * 128) / width - 128;
            int x0 = sx < 0 ? 0 : (int)(sx >> 8), fx = sx < 0 ? 0 : (int)(sx & 255);
            int x1 = x0 + 1 < source->width ? x0 + 1 : x0;
            uint32_t a = source->pixels[y0 * source->width + x0], b = source->pixels[y0 * source->width + x1];
            uint32_t c = source->pixels[y1 * source->width + x0], d = source->pixels[y1 * source->width + x1];
            uint32_t out = 0;

            for (int shift = 0; shift < 32; shift += 8) {
                int top = (a >> shift & 0xFF) * (256 - fx) + (b >> shift & 0xFF) * fx;
                int bottom = (c >> shift & 0xFF) * (256 - fx) + (d >> shift & 0xFF) * fx;

                out |= (uint32_t)((top * (256 - fy) + bottom * fy + 32768) >> 16) << shift;
            }
            to->pixels[y * width + x] = out;
        }
    }
    gui_image_free(&smaller);
    return true;
}

// In the canvas's own pixels, each from the nearest of the image's.
void gui_draw_image(struct gui_canvas *canvas, int x, int y, const struct gui_image *image) {
    int k = gui_scale_of(canvas), own = image->scale > 1 ? image->scale : 1;
    int width = image->width * k / own, height = image->height * k / own;

    x *= k, y *= k;

    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + width > canvas->width * k ? canvas->width * k - x : width;
    int y1 = y + height > canvas->height * k ? canvas->height * k - y : height;

    for (int row = y0; row < y1; row++) {
        const uint32_t *from = image->pixels + (long)(row * own / k) * image->width;
        uint32_t *to = canvas->pixels + (long)(y + row) * canvas->stride + x;

        // Red and blue together, then green, with alpha out of 256 so that the
        // shift keeps the channels apart.
        for (int col = x0; col < x1; col++) {
            uint32_t p = from[col * own / k], a = (p >> 24) + (p >> 31), q = to[col];

            if (a == 256)
                to[col] = p & 0xFFFFFF;
            else if (a)
                to[col] = (((p & 0xFF00FF) * a + (q & 0xFF00FF) * (256 - a)) >> 8 & 0xFF00FF)
                        | (((p & 0x00FF00) * a + (q & 0x00FF00) * (256 - a)) >> 8 & 0x00FF00);
        }
    }
}

#define ICONS GUI_SHARE "/icons"
#define MAX_ICONS 64

static int icon_scale = 1;

void gui_use_scale(int scale) {
    icon_scale = scale > 1 ? scale : 1;
}

// At 2x, the icon drawn for it, or the 1x one, which is drawn twice the size.
const struct gui_image *gui_icon(const char *name) {
    static struct {
        char name[32];
        int scale;
        struct gui_image image; // no pixels when there's no such icon
    } icons[MAX_ICONS];
    static int count;
    char path[sizeof(ICONS) + 40];

    for (int i = 0; i < count; i++) {
        if (!strcmp(icons[i].name, name) && icons[i].scale == icon_scale)
            return icons[i].image.pixels ? &icons[i].image : NULL;
    }
    if (count == MAX_ICONS || strlen(name) >= sizeof(icons[0].name))
        return NULL;
    strcpy(icons[count].name, name);
    icons[count].scale = icon_scale;
    snprintf(path, sizeof(path), ICONS "/%s@2x.png", name);
    if (icon_scale >= 2 && gui_image_load(path, &icons[count].image))
        icons[count].image.scale = 2;
    else {
        snprintf(path, sizeof(path), ICONS "/%s.png", name);
        gui_image_load(path, &icons[count].image);
    }
    return icons[count++].image.pixels ? &icons[count - 1].image : NULL;
}

const struct gui_image *gui_file_icon(const char *name, bool folder) {
    static const char *const images[] = {"png", "jpg", "jpeg", "bmp", "gif", "webp", NULL};
    static const char *const texts[] = {"txt", "md", "c", "h", "py", "sh", "conf", "log", "json", "html", "css", NULL};
    static const char *const sounds[] = {"wav", "mp3", "flac", "ogg", "oga", "opus", "aac", NULL};
    const char *dot = strrchr(name, '.');

    if (folder)
        return gui_icon("folder");
    for (int i = 0; dot && images[i]; i++) {
        if (!strcasecmp(dot + 1, images[i]))
            return gui_icon("image");
    }
    for (int i = 0; dot && texts[i]; i++) {
        if (!strcasecmp(dot + 1, texts[i]))
            return gui_icon("text");
    }
    for (int i = 0; dot && sounds[i]; i++) {
        if (!strcasecmp(dot + 1, sounds[i]))
            return gui_icon("audio");
    }
    return gui_icon("file");
}
