#include "vt.h"
#include <errno.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// An xterm, for TERM=xterm-256color: UTF-8, with wide characters two cells wide,
// 256 colours and true colour, the alternate screen, scroll regions, inserting and
// deleting lines and characters, and replies to what programs ask the terminal.

#define TAB_STOP 8

struct session *t;
int cols = 80, rows = 25;
bool dirty[MAX_ROWS];
uint32_t colors[256], default_fg, default_bg;

// --- Character widths, as programs measure them: two for East Asian wide ones and
// emoji shown as pictures, none for combining marks and joiners ---

struct range {
    uint32_t from, to;
};

static const struct range zero_width[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064},
    {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0000, 0xE01EF},
};

static const struct range wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static bool in(uint32_t cp, const struct range *ranges, size_t count) {
    size_t low = 0, high = count;

    while (low < high) {
        size_t mid = (low + high) / 2;

        if (cp > ranges[mid].to)
            low = mid + 1;
        else if (cp < ranges[mid].from)
            high = mid;
        else
            return true;
    }
    return false;
}

int char_width(uint32_t cp) {
    if (cp == 0x200D || in(cp, zero_width, sizeof(zero_width) / sizeof(zero_width[0])))
        return 0;
    return in(cp, wide, sizeof(wide) / sizeof(wide[0])) ? 2 : 1;
}

// --- The screen ---

void mark_all_dirty(void) {
    for (int r = 0; r < rows; r++)
        dirty[r] = true;
}

struct cell *history_line(int i) {
    return t->history[(t->history_start + i) % SCROLLBACK];
}

static struct cell blank(void) {
    return (struct cell){' ', t->pen.fg, t->pen.bg}; // erased cells take the background being drawn with
}

static void clear_cells(int r, int from, int to) {
    struct cell b = blank();

    for (int c = from < 0 ? 0 : from; c < to && c < MAX_COLS; c++)
        t->cells[r][c] = b;
    dirty[r] = true;
}

// Keeps a line leaving the top of the screen, losing the oldest when full. A view
// scrolled back stays on the lines it shows.
bool line_wraps(const struct cell *line) {
    return line[MAX_COLS - 1].ch & LINE_WRAPS;
}

static void line_feed(void);

// The cursor's line goes on in the next, as it's wrapped.
static void wrap(void) {
    t->cells[t->row][MAX_COLS - 1].ch |= LINE_WRAPS;
    t->col = 0;
    line_feed();
}

void keep_line(const struct cell *line) {
    struct cell *to = history_line(t->history_count);

    if (t->history_count < SCROLLBACK) {
        t->history_count++;
    } else {
        // Every kept line moves up one, and the selection with them.
        t->history_start = (t->history_start + 1) % SCROLLBACK;
        t->select_from.line--;
        t->select_to.line--;
        t->selected &= t->select_from.line >= 0 && t->select_to.line >= 0;
    }
    memcpy(to, line, cols * sizeof(*line));
    for (int c = cols; c < MAX_COLS; c++)
        to[c] = (struct cell){' ', COLOR_DEFAULT, COLOR_DEFAULT};
    to[MAX_COLS - 1].ch |= line[MAX_COLS - 1].ch & LINE_WRAPS;
    if (t->view) {
        t->view = t->view < t->history_count ? t->view + 1 : t->history_count;
        mark_all_dirty();
    }
}

// The scroll region's lines move up n, those leaving the top of the whole primary
// screen kept, and blank ones come in at the bottom. Down is the other way.
static void scroll_up(int n) {
    int height = t->bottom - t->top + 1;

    n = n > height ? height : n;
    for (int i = 0; i < n && t->top == 0 && !t->alternate_screen; i++)
        keep_line(t->cells[i]);
    memmove(t->cells[t->top], t->cells[t->top + n], (height - n) * sizeof(t->cells[0]));
    for (int r = t->bottom - n + 1; r <= t->bottom; r++)
        clear_cells(r, 0, MAX_COLS);
    for (int r = t->top; r <= t->bottom; r++)
        dirty[r] = true;
}

static void scroll_down(int n) {
    int height = t->bottom - t->top + 1;

    n = n > height ? height : n;
    memmove(t->cells[t->top + n], t->cells[t->top], (height - n) * sizeof(t->cells[0]));
    for (int r = t->top; r < t->top + n; r++)
        clear_cells(r, 0, MAX_COLS);
    for (int r = t->top; r <= t->bottom; r++)
        dirty[r] = true;
}

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static void move_to(int row, int col) {
    dirty[t->row] = true;
    t->row = clamp(row, 0, rows - 1);
    t->col = clamp(col, 0, cols - 1);
    t->wrap_next = false;
    dirty[t->row] = true;
}

static void line_feed(void) {
    if (t->row == t->bottom)
        scroll_up(1);
    else
        move_to(t->row + 1, t->col);
}

static void reverse_index(void) {
    if (t->row == t->top)
        scroll_down(1);
    else
        move_to(t->row - 1, t->col);
}

// A wide character half overwritten loses its other half.
static void break_wide(int r, int c) {
    if (c < 0 || c >= cols)
        return;
    if (t->cells[r][c].ch & ATTR_WIDE && c + 1 < cols)
        t->cells[r][c + 1] = blank();
    if ((t->cells[r][c].ch & CH_MASK) == 0 && c > 0 && t->cells[r][c - 1].ch & ATTR_WIDE)
        t->cells[r][c - 1] = blank();
}

// DEC's line-drawing characters, which programs switch to for boxes.
static uint32_t line_drawing(uint32_t c) {
    static const char *const from = "`afgjklmnopqrstuvwxyz{|}~";
    static const uint32_t to[] = {0x25C6, 0x2592, 0x00B0, 0x00B1, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB,
                                  0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0,
                                  0x2260, 0x00A3, 0x00B7};
    const char *at = c < 0x80 ? strchr(from, (int)c) : NULL;

    return at && c ? to[at - from] : c;
}

static void put_char(uint32_t cp) {
    int width;

    if (t->line_drawing)
        cp = line_drawing(cp);
    if (!(width = char_width(cp)))
        return; // marks and joiners have no place of their own here
    if (t->wrap_next && t->autowrap)
        wrap();
    if (width == 2 && t->col == cols - 1) { // doesn't fit: blank the last cell, and wrap
        if (!t->autowrap)
            return;
        clear_cells(t->row, t->col, t->col + 1);
        wrap();
    }

    struct cell *line = t->cells[t->row];

    if (t->insert && cols - t->col - width > 0)
        memmove(&line[t->col + width], &line[t->col], (cols - t->col - width) * sizeof(*line));
    break_wide(t->row, t->col);
    if (width == 2)
        break_wide(t->row, t->col + 1);
    line[t->col] = (struct cell){cp | (t->pen.ch & ~CH_MASK & ~ATTR_WIDE) | (width == 2 ? ATTR_WIDE : 0), t->pen.fg,
                                 t->pen.bg};
    if (width == 2)
        line[t->col + 1] = (struct cell){t->pen.ch & ~CH_MASK & ~ATTR_WIDE, t->pen.fg, t->pen.bg};
    dirty[t->row] = true;
    t->last = cp;
    if (t->col + width >= cols) {
        t->col = cols - 1;
        t->wrap_next = t->autowrap;
    } else {
        t->col += width;
    }
}

void vt_reply(const char *text) {
    size_t length = strlen(text);

    while (length) {
        ssize_t n = write(t->master, text, length);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        text += n;
        length -= n;
    }
}

static void save_cursor(void) {
    struct saved_cursor *s = t->alternate_screen ? &t->alternate_saved : &t->saved;

    *s = (struct saved_cursor){t->row, t->col, t->pen, t->line_drawing, t->wrap_next};
}

static void restore_cursor(void) {
    struct saved_cursor *s = t->alternate_screen ? &t->alternate_saved : &t->saved;

    move_to(s->row, s->col);
    t->pen = s->pen;
    t->line_drawing = s->line_drawing;
    t->wrap_next = s->wrap_next;
}

static void use_alternate(bool on, bool clear) {
    if (on == t->alternate_screen)
        return;
    t->alternate_screen = on;
    t->cells = on ? t->alternate : t->primary;
    if (on && clear)
        for (int r = 0; r < MAX_ROWS; r++)
            clear_cells(r, 0, MAX_COLS);
    t->view = 0;
    mark_all_dirty();
}

void vt_init(struct session *s) {
    struct session *was = t;

    t = s;
    s->cells = s->primary;
    s->autowrap = true;
    s->top = 0;
    s->bottom = rows - 1;
    s->pen = (struct cell){0, COLOR_DEFAULT, COLOR_DEFAULT};
    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++)
            s->primary[r][c] = s->alternate[r][c] = (struct cell){' ', COLOR_DEFAULT, COLOR_DEFAULT};
    }
    t = was ? was : s;
}

static void reset(void) {
    use_alternate(false, false);
    t->pen = (struct cell){0, COLOR_DEFAULT, COLOR_DEFAULT};
    t->top = 0;
    t->bottom = rows - 1;
    t->cursor_hidden = t->app_cursor = t->insert = t->bracketed_paste = t->focus_events = t->line_drawing = false;
    t->mouse_sgr = false;
    t->mouse = 0;
    t->autowrap = true;
    for (int r = 0; r < rows; r++)
        clear_cells(r, 0, MAX_COLS);
    move_to(0, 0);
}

// --- Escape sequences ---

static int param(int index, int otherwise) {
    return index < t->param_count && t->params[index] > 0 ? t->params[index] : otherwise;
}

// A colour after 38 or 48: 5 and an index, or 2 and red, green and blue, which with
// colons may have an empty colour space before them. Returns how many it took.
static int extended_color(int i, uint32_t *out) {
    if (i + 1 < t->param_count && t->params[i + 1] == 5 && i + 2 < t->param_count) {
        *out = COLOR_INDEX | (t->params[i + 2] & 0xFF);
        return 2;
    }
    if (i + 1 < t->param_count && t->params[i + 1] == 2) {
        int first = i + 2;

        if (t->colon[i + 1] && i + 5 < t->param_count && t->colon[i + 5])
            first++; // the colour space, unused
        if (first + 2 < t->param_count) {
            *out = COLOR_RGB | (t->params[first] & 0xFF) << 16 | (t->params[first + 1] & 0xFF) << 8 | (t->params[first + 2] & 0xFF);
            return first + 2 - i;
        }
    }
    return t->param_count - i;
}

static void graphics(void) {
    struct cell *p = &t->pen;

    if (!t->param_count)
        t->params[t->param_count++] = 0;
    for (int i = 0; i < t->param_count; i++) {
        int n = t->params[i];

        if (n == 0)
            *p = (struct cell){0, COLOR_DEFAULT, COLOR_DEFAULT};
        else if (n == 1)
            p->ch |= ATTR_BOLD;
        else if (n == 2)
            p->ch |= ATTR_DIM;
        else if (n == 3)
            p->ch |= ATTR_ITALIC;
        else if (n == 4)
            p->ch = (i + 1 < t->param_count && t->colon[i + 1] && t->params[i + 1] == 0) ? p->ch & ~ATTR_UNDERLINE
                                                                                          : p->ch | ATTR_UNDERLINE;
        else if (n == 7)
            p->ch |= ATTR_REVERSE;
        else if (n == 8)
            p->ch |= ATTR_HIDDEN;
        else if (n == 9)
            p->ch |= ATTR_STRIKE;
        else if (n == 21 || n == 24)
            p->ch &= ~ATTR_UNDERLINE;
        else if (n == 22)
            p->ch &= ~(ATTR_BOLD | ATTR_DIM);
        else if (n == 23)
            p->ch &= ~ATTR_ITALIC;
        else if (n == 27)
            p->ch &= ~ATTR_REVERSE;
        else if (n == 28)
            p->ch &= ~ATTR_HIDDEN;
        else if (n == 29)
            p->ch &= ~ATTR_STRIKE;
        else if (n >= 30 && n <= 37)
            p->fg = COLOR_INDEX | (n - 30);
        else if (n == 38)
            i += extended_color(i, &p->fg);
        else if (n == 39)
            p->fg = COLOR_DEFAULT;
        else if (n >= 40 && n <= 47)
            p->bg = COLOR_INDEX | (n - 40);
        else if (n == 48)
            i += extended_color(i, &p->bg);
        else if (n == 49)
            p->bg = COLOR_DEFAULT;
        else if (n == 58)
            i += extended_color(i, &(uint32_t){0}); // underline colour, which is the text's here
        else if (n >= 90 && n <= 97)
            p->fg = COLOR_INDEX | (n - 90 + 8);
        else if (n >= 100 && n <= 107)
            p->bg = COLOR_INDEX | (n - 100 + 8);
    }
}

static bool *private_mode(int mode) {
    switch (mode) {
    case 1: return &t->app_cursor;
    case 7: return &t->autowrap;
    case 1004: return &t->focus_events;
    case 1006: return &t->mouse_sgr;
    case 2004: return &t->bracketed_paste;
    }
    return NULL;
}

static void set_mode(int mode, bool on) {
    bool *flag = private_mode(mode);

    if (flag)
        *flag = on;
    else if (mode == 25)
        t->cursor_hidden = !on, dirty[t->row] = true;
    else if (mode == 47 || mode == 1047)
        use_alternate(on, on);
    else if (mode == 1048)
        on ? save_cursor() : restore_cursor();
    else if (mode == 1049) {
        if (on)
            save_cursor(), use_alternate(true, true);
        else
            use_alternate(false, false), restore_cursor();
    } else if (mode == 1000 || mode == 1002 || mode == 1003)
        t->mouse = on ? mode : 0;
}

// DECRQM: whether a mode's set, 1, reset, 2, or unknown, 0.
static void report_mode(int mode) {
    bool *flag = private_mode(mode);
    int state = flag ? (*flag ? 1 : 2)
              : mode == 25 ? (t->cursor_hidden ? 2 : 1)
              : mode == 1049 || mode == 47 || mode == 1047 ? (t->alternate_screen ? 1 : 2)
              : mode == 1000 || mode == 1002 || mode == 1003 ? (t->mouse == mode ? 1 : 2)
              : mode == 2026 ? 2 // synchronized output, which needs nothing doing here
                             : 0;
    char reply[32];

    snprintf(reply, sizeof(reply), "\x1b[?%d;%d$y", mode, state);
    vt_reply(reply);
}

static void csi(char final) {
    char reply[64];
    int n = param(0, 1);

    if (t->prefix == '?' && (final == 'h' || final == 'l')) {
        for (int i = 0; i < (t->param_count ? t->param_count : 1); i++)
            set_mode(t->params[i], final == 'h');
        return;
    }
    if (t->prefix == '?' && t->intermediate == '$' && final == 'p') {
        report_mode(t->params[0]);
        return;
    }
    if (t->prefix == '>' && final == 'c') { // the second device attributes: a VT220, of version 10
        vt_reply("\x1b[>1;10;0c");
        return;
    }
    if (t->prefix == '>' && final == 'q') { // XTVERSION
        vt_reply("\x1bP>|AximOS Terminal\x1b\\");
        return;
    }
    if (t->prefix || t->intermediate)
        return; // the others, as cursor shapes and keyboard modes, which it hasn't

    switch (final) {
    case 'A':
        move_to(t->row >= t->top && t->row - n < t->top ? t->top : t->row - n, t->col);
        break;
    case 'B':
    case 'e':
        move_to(t->row <= t->bottom && t->row + n > t->bottom ? t->bottom : t->row + n, t->col);
        break;
    case 'C':
    case 'a':
        move_to(t->row, t->col + n);
        break;
    case 'D':
        move_to(t->row, t->col - n);
        break;
    case 'E':
        move_to(t->row + n, 0);
        break;
    case 'F':
        move_to(t->row - n, 0);
        break;
    case 'G':
    case '`':
        move_to(t->row, n - 1);
        break;
    case 'd':
        move_to(n - 1, t->col);
        break;
    case 'H':
    case 'f':
        move_to(param(0, 1) - 1, param(1, 1) - 1);
        break;
    case 'J':
        if (t->params[0] == 3) { // and what's scrolled off, as clear does
            t->history_count = t->view = 0;
            mark_all_dirty();
            break;
        }
        if (t->params[0] == 0 || t->params[0] == 2) {
            for (int r = t->params[0] == 2 ? 0 : t->row + 1; r < rows; r++)
                clear_cells(r, 0, MAX_COLS);
        }
        if (t->params[0] == 1)
            for (int r = 0; r < t->row; r++)
                clear_cells(r, 0, MAX_COLS);
        if (t->params[0] == 0)
            clear_cells(t->row, t->col, MAX_COLS);
        else if (t->params[0] == 1)
            clear_cells(t->row, 0, t->col + 1);
        break;
    case 'K':
        clear_cells(t->row, t->params[0] == 0 ? t->col : 0, t->params[0] == 1 ? t->col + 1 : MAX_COLS);
        break;
    case 'X':
        break_wide(t->row, t->col);
        clear_cells(t->row, t->col, t->col + n);
        break;
    case '@':
        n = n > cols - t->col ? cols - t->col : n;
        memmove(&t->cells[t->row][t->col + n], &t->cells[t->row][t->col], (cols - t->col - n) * sizeof(struct cell));
        clear_cells(t->row, t->col, t->col + n);
        break;
    case 'P':
        n = n > cols - t->col ? cols - t->col : n;
        memmove(&t->cells[t->row][t->col], &t->cells[t->row][t->col + n], (cols - t->col - n) * sizeof(struct cell));
        clear_cells(t->row, cols - n, cols);
        break;
    case 'L':
    case 'M':
        if (t->row >= t->top && t->row <= t->bottom) {
            int top = t->top;

            t->top = t->row;
            final == 'L' ? scroll_down(n) : scroll_up(n);
            t->top = top;
            move_to(t->row, 0);
        }
        break;
    case 'S':
        scroll_up(n);
        break;
    case 'T':
        scroll_down(n);
        break;
    case 'b':
        for (int i = 0; i < n && t->last; i++)
            put_char(t->last);
        break;
    case 'r': {
        int top = param(0, 1) - 1, bottom = param(1, rows) - 1;

        if (top < bottom && bottom < rows) {
            t->top = top;
            t->bottom = bottom;
            move_to(0, 0);
        }
        break;
    }
    case 's':
        save_cursor();
        break;
    case 'u':
        restore_cursor();
        break;
    case 'm':
        graphics();
        break;
    case 'h':
    case 'l':
        if (t->params[0] == 4)
            t->insert = final == 'h';
        break;
    case 'n':
        if (t->params[0] == 5)
            vt_reply("\x1b[0n");
        else if (t->params[0] == 6) {
            snprintf(reply, sizeof(reply), "\x1b[%d;%dR", t->row + 1, t->col + 1);
            vt_reply(reply);
        }
        break;
    case 'c': // a VT220, with colour
        if (t->params[0] == 0)
            vt_reply("\x1b[?62;22c");
        break;
    case 't':
        if (t->params[0] == 18) {
            snprintf(reply, sizeof(reply), "\x1b[8;%d;%dt", rows, cols);
            vt_reply(reply);
        }
        break;
    }
}

// A colour as "rgb:RRRR/GGGG/BBBB", as xterm answers what colour it is.
static void color_reply(const char *code, uint32_t rgb, bool bell) {
    char reply[64];

    snprintf(reply, sizeof(reply), "\x1b]%s;rgb:%02x%02x/%02x%02x/%02x%02x%s", code, rgb >> 16 & 0xFF, rgb >> 16 & 0xFF,
             rgb >> 8 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, rgb & 0xFF, bell ? "\a" : "\x1b\\");
    vt_reply(reply);
}

static void base64_to_clipboard(const char *text) {
    static const char *const letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen(text), n = 0;
    char *out = malloc(length * 3 / 4 + 3);
    uint32_t bits = 0;
    int have = 0;

    if (!out)
        return;
    for (; *text; text++) {
        const char *at = strchr(letters, *text);

        if (!at || !*at)
            continue;
        bits = bits << 6 | (uint32_t)(at - letters);
        if ((have += 6) >= 8)
            out[n++] = (char)(bits >> (have -= 8));
    }
    gui_clipboard_set(out, n);
    free(out);
}

// OSC: the title, what colour the text and background are, and copying.
static void osc(bool bell) {
    char *text = t->osc, *rest;
    int code = atoi(text);

    t->osc[t->osc_length] = '\0';
    if (!(rest = strchr(text, ';')))
        return;
    rest++;
    if (code == 0 || code == 2) {
        snprintf(t->title, sizeof(t->title), "%s", rest);
        vt_title_changed();
    } else if ((code == 10 || code == 11) && !strcmp(rest, "?")) {
        color_reply(code == 10 ? "10" : "11", (code == 10 ? default_fg : default_bg) & 0xFFFFFF, bell);
    } else if (code == 4 && strstr(rest, ";?")) {
        char which[16];
        int index = atoi(rest) & 0xFF;

        snprintf(which, sizeof(which), "4;%d", index);
        color_reply(which, colors[index], bell);
    } else if (code == 52 && strchr(rest, ';') && strcmp(strchr(rest, ';') + 1, "?")) {
        base64_to_clipboard(strchr(rest, ';') + 1);
    }
}

static void begin(int state) {
    t->state = state;
    t->param_count = 0;
    t->prefix = t->intermediate = 0;
    t->osc_length = 0;
    memset(t->params, 0, sizeof(t->params));
    memset(t->colon, 0, sizeof(t->colon));
}

static void escape(char c) {
    t->state = TEXT;
    switch (c) {
    case '[': begin(CSI); break;
    case ']': begin(OSC); break;
    case 'P': case '_': case '^': case 'X': begin(DCS); break; // DCS, APC, PM and SOS, which it passes over
    case '(': t->state = CHARSET; break;
    case ')': case '*': case '+': case '-': case '.': case '/': t->state = CHARSET; t->intermediate = 'x'; break;
    case '7': save_cursor(); break;
    case '8': restore_cursor(); break;
    case 'D': line_feed(); break;
    case 'E': t->col = 0, line_feed(); break;
    case 'M': reverse_index(); break;
    case 'c': reset(); break;
    }
}

static void control(uint32_t c) {
    switch (c) {
    case '\n': case '\v': case '\f':
        line_feed();
        break;
    case '\r':
        move_to(t->row, 0);
        break;
    case '\b':
        move_to(t->row, t->col - 1);
        break;
    case '\t':
        move_to(t->row, (t->col / TAB_STOP + 1) * TAB_STOP);
        break;
    case 0x1B:
        t->state = ESCAPE;
        break;
    }
}

static void text_char(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F)
        control(cp);
    else if (cp >= 0x80 && cp < 0xA0)
        ; // C1 controls, which programs send as escapes instead
    else
        put_char(cp);
}

// UTF-8, with anything that isn't as U+FFFD.
static void text_byte(uint8_t b) {
    if (t->partial_left && (b & 0xC0) == 0x80) {
        t->partial = t->partial << 6 | (b & 0x3F);
        if (!--t->partial_left)
            text_char(t->partial > 0x10FFFF ? 0xFFFD : t->partial);
        return;
    }
    if (t->partial_left) { // cut short
        t->partial_left = 0;
        text_char(0xFFFD);
    }
    if (b < 0x80)
        text_char(b);
    else if ((b & 0xE0) == 0xC0)
        t->partial = b & 0x1F, t->partial_left = 1;
    else if ((b & 0xF0) == 0xE0)
        t->partial = b & 0x0F, t->partial_left = 2;
    else if ((b & 0xF8) == 0xF0)
        t->partial = b & 0x07, t->partial_left = 3;
    else
        text_char(0xFFFD);
}

static void csi_byte(char c) {
    if (c >= '0' && c <= '9') {
        if (!t->param_count)
            t->param_count = 1;
        if (t->param_count <= MAX_PARAMS)
            t->params[t->param_count - 1] = t->params[t->param_count - 1] * 10 + (c - '0');
    } else if (c == ';' || c == ':') {
        if (!t->param_count)
            t->param_count = 1;
        if (t->param_count < MAX_PARAMS) {
            t->colon[t->param_count] = c == ':';
            t->param_count++;
        }
    } else if (c == '?' || c == '>' || c == '<' || c == '=') {
        t->prefix = c;
    } else if (c >= 0x20 && c <= 0x2F) {
        t->intermediate = c;
    } else if (c >= 0x40 && c <= 0x7E) {
        t->state = TEXT;
        csi(c);
    } else if (c == 0x1B) {
        t->state = ESCAPE;
    }
}

void vt_write(const char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = bytes[i];

        switch (t->state) {
        case TEXT:
            text_byte((uint8_t)c);
            break;
        case ESCAPE:
            escape(c);
            break;
        case CHARSET:
            if (!t->intermediate) // G0, the one used: DEC's line drawing, or ASCII
                t->line_drawing = c == '0';
            t->state = TEXT;
            break;
        case CSI:
            csi_byte(c);
            break;
        case OSC:
            if (c == '\a') {
                osc(true);
                t->state = TEXT;
            } else if (c == 0x1B) {
                t->state = OSC_ESCAPE;
            } else if (t->osc_length < (int)sizeof(t->osc) - 1) {
                t->osc[t->osc_length++] = c;
            }
            break;
        case OSC_ESCAPE: // ESC \ ends it
            osc(false);
            t->state = TEXT;
            if (c != '\\')
                escape(c);
            break;
        case DCS: // passed over, to its ESC \.
            if (c == 0x1B)
                t->state = DCS_ESCAPE;
            break;
        case DCS_ESCAPE:
            t->state = c == '\\' ? TEXT : DCS;
            break;
        }
    }
}
