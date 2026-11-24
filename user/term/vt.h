#pragma once

#include <stdint.h>
#include <sys/types.h>

// The terminal each tab is: xterm's, as TERM=xterm-256color says, kept in vt.c.
// term.c draws it, and takes the window's keys, mouse and tabs.

#define MAX_COLS 320
#define MAX_ROWS 200
#define SCROLLBACK 1000 // lines kept after scrolling off the top
#define MAX_PARAMS 16

// A cell: its character, 0 in the second cell of a wide one, with its attributes
// above it, and its colours.
#define CH_MASK 0x1FFFFF
#define ATTR_BOLD (1u << 24)
#define ATTR_DIM (1u << 25)
#define ATTR_UNDERLINE (1u << 26)
#define ATTR_REVERSE (1u << 27)
#define ATTR_HIDDEN (1u << 28)
#define ATTR_STRIKE (1u << 29)
#define ATTR_WIDE (1u << 30) // the first cell of a character two wide
#define ATTR_ITALIC (1u << 31) // kept, though there's no italic font
// A colour: the default, one of the 256, or red, green and blue.
#define COLOR_DEFAULT 0
#define COLOR_INDEX (1u << 24)
#define COLOR_RGB (2u << 24)

struct cell {
    uint32_t ch, fg, bg;
};

// What the mouse has selected, from where it was pressed to where it's been
// dragged, by line counting from the oldest kept, and column.
struct place {
    int line, col;
};

struct saved_cursor {
    int row, col;
    struct cell pen;
    bool line_drawing, wrap_next;
};

// Each tab is a program on a terminal of its own, with its screen, the alternate
// screen full-screen programs use, the lines that scrolled off the top of the
// first, oldest first from history_start, and how many the view's scrolled back.
struct session {
    int master;
    pid_t child;
    int number; // in the order they were opened, for its name
    struct cell (*cells)[MAX_COLS]; // the screen showing, one of these
    struct cell primary[MAX_ROWS][MAX_COLS], alternate[MAX_ROWS][MAX_COLS];
    struct cell history[SCROLLBACK][MAX_COLS];
    int history_start, history_count, view;
    struct place select_from, select_to;
    bool selected;

    int row, col, top, bottom; // the cursor, and the rows that scroll
    bool wrap_next;            // past the last column, it goes to the next line with what comes next
    struct cell pen;           // what the next character's drawn with
    struct saved_cursor saved, alternate_saved;
    uint32_t last; // the last character, which REP repeats
    char title[128];

    // Modes programs set
    bool alternate_screen, cursor_hidden, app_cursor, autowrap, insert, bracketed_paste, focus_events, line_drawing,
        mouse_sgr;
    int mouse; // 0, or 1000 for clicks, 1002 with dragging too, 1003 with every move

    // The escape sequence or UTF-8 character being read
    enum { TEXT, ESCAPE, CHARSET, CSI, OSC, OSC_ESCAPE, DCS, DCS_ESCAPE } state;
    int params[MAX_PARAMS], param_count;
    bool colon[MAX_PARAMS]; // the parameter came after a colon, as in 38:2::r:g:b
    char prefix, intermediate;
    char osc[1024];
    int osc_length;
    uint32_t partial;
    int partial_left;
};

extern struct session *t; // the one being written to, or shown
extern int cols, rows;
extern bool dirty[MAX_ROWS]; // rows of the screen to draw again

// The 256 colours, the first 16 the theme's, and the defaults.
extern uint32_t colors[256], default_fg, default_bg;

void vt_init(struct session *s);
void vt_write(const char *bytes, size_t length); // to t, from its program
void vt_reply(const char *text);                 // to t's program, as if typed
struct cell *history_line(int i);
void keep_line(const struct cell *line);
void mark_all_dirty(void);
int char_width(uint32_t cp); // in cells: 0, 1 or 2
void vt_title_changed(void); // term.c's, when t's title's changed
