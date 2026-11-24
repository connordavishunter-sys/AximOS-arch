#pragma once

#include <gui.h>

// Whether text is drawn smoothly, from the settings, which is the default.
extern bool font_smooth;

// Draw text smoothly, monospaced or in DejaVu Sans, returning false if they can't,
// when the bitmap font is used.
bool font_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
bool font_sans_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);

// How wide a character is in DejaVu Sans, or -1 if it isn't used.
int font_sans_advance(unsigned char c, int scale);

// A character in a terminal's cells, cells wide, from the monospaced font, its bold,
// or DejaVu Sans for those it hasn't, returning false if there's no font with it.
bool font_cell(struct gui_canvas *canvas, int x, int y, uint32_t cp, int cells, bool bold, uint32_t color);
