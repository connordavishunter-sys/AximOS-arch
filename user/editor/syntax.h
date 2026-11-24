#pragma once

#include <gui.h>

// How to colour a file: C for .c and .h, the shell for .sh or a #! line naming
// one, and NULL, for plain text, otherwise.
gui_highlighter *syntax_for(const char *path, const char *text);
const char *syntax_name(gui_highlighter *highlight);
