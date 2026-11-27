#include "vt.h"
#include <aximos.h>
#include <ctype.h>
#include <errno.h>
#include <gui.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

// The window: tabs, each a terminal from vt.c with a program on it, drawn in cells,
// with the scrollback, selecting and copying, finding, and a menu. Keys go to the
// program as xterm sends them, and the mouse too, when the program asks for it.

#define COLS 80 // to start with
#define ROWS 25
#define CELL_WIDTH GUI_CHAR_WIDTH
#define CELL_HEIGHT GUI_CHAR_HEIGHT
#define PADDING 4
#define WHEEL_LINES 3
#define DRAG_SCROLL_MS 50 // between scrolls while a selection is dragged out of the window
#define KEY_C 0x2E
#define KEY_V 0x2F
#define KEY_T 0x14
#define KEY_W 0x11
#define KEY_F 0x21
#define MAX_TABS 8
#define FIND "##find"
#define FIND_BUTTONS 300 // beside the find bar's box: Older, Newer, how it went, and Close
#define WIDGET_MARGIN 10 // libgui's, around its widgets
#define FIND_BAR (GUI_CHAR_HEIGHT + 10 + 2 * WIDGET_MARGIN) // a text box's height, and its margins
#define KEY_Q 0x10
// How see-through the default background is, in its top byte, which wm reads: as much as title bars.
#define SEE_THROUGH 0x38000000

// The console's 16 colours, which the rest of the 256 follow.
static const uint32_t base_colors[16] = {
    0x000000, 0xCC3333, 0x33AA33, 0xCCAA33, 0x3366CC, 0xAA44AA, 0x33AAAA, 0xC0C0C0,
    0x666666, 0xFF6666, 0x66DD66, 0xFFDD66, 0x6699FF, 0xDD77DD, 0x66DDDD, 0xFFFFFF,
};

static struct gui_window *window;
static struct session *sessions[MAX_TABS];
static int session_count, active;
static struct gui_ui ui;
static struct gui_rect area;        // where the cells go, under the tabs and over the find bar
static bool stale = true;           // the window's to be drawn again, libgui's widgets and all
static bool mouse_on_widgets;       // the mouse was last over libgui's, rather than the cells
static int left = PADDING, top = PADDING; // the first cell's top left
static bool focused, selecting;
static int drag_x, drag_y; // where the mouse is while selecting
static long last_press;    // for double clicks
static uint8_t modifiers;  // held, as the last key said, which mouse events don't
static int mouse_button = -1; // held, for reporting drags to the program
static volatile sig_atomic_t dump_asked;

// libgui's, around the cells: the tabs, a menu from a right-click, finding what's
// kept, in a bar under the cells, and the help.
static bool searching, search_started;
static char query[64], search_status[64];
static int found_line = -1;

static const char *const help = "Ctrl+Shift+C, V      Copy and paste\n"
                                "Ctrl+Shift+T, W      New tab, close the tab\n"
                                "Ctrl+Tab             Next tab\n"
                                "Ctrl+Shift+F         Find in what's kept\n"
                                "Enter, Shift+Enter   Find older, newer\n"
                                "Shift+Page Up, Down  Scroll back and forth\n"
                                "Double click         Select a word\n"
                                "Shift+drag           Select, while a program has the mouse\n"
                                "F1                   This help";

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

static long now_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void write_all(int fd, const char *s, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, s, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        s += n;
        len -= n;
    }
}

static size_t utf8(uint32_t cp, char *out) {
    if (cp < 0x80)
        return out[0] = (char)cp, 1;
    if (cp < 0x800)
        return out[0] = (char)(0xC0 | cp >> 6), out[1] = (char)(0x80 | (cp & 0x3F)), 2;
    if (cp < 0x10000)
        return out[0] = (char)(0xE0 | cp >> 12), out[1] = (char)(0x80 | (cp >> 6 & 0x3F)), out[2] = (char)(0x80 | (cp & 0x3F)), 3;
    out[0] = (char)(0xF0 | cp >> 18), out[1] = (char)(0x80 | (cp >> 12 & 0x3F)), out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// --- Colours ---

// The 16, then a 6 by 6 by 6 cube, then 24 greys, as xterm's 256.
static void make_colors(void) {
    static const uint8_t level[6] = {0, 95, 135, 175, 215, 255};

    memcpy(colors, base_colors, sizeof(base_colors));
    for (int i = 0; i < 216; i++)
        colors[16 + i] = level[i / 36] << 16 | level[i / 6 % 6] << 8 | level[i % 6];
    for (int i = 0; i < 24; i++)
        colors[232 + i] = (8 + i * 10) * 0x010101;
}

static uint32_t resolve(uint32_t color, bool is_fg, bool bold) {
    if (color == COLOR_DEFAULT)
        return is_fg ? default_fg : default_bg;
    if ((color & 0xFF000000) == COLOR_RGB)
        return color & 0xFFFFFF;

    int index = color & 0xFF;

    return colors[is_fg && bold && index < 8 ? index + 8 : index]; // bold shows brighter, as on the console
}

static void use_theme(void) {
    struct gui_settings settings;

    gui_settings_load(&settings);
    default_fg = settings.light ? 0x1E2129 : 0xD8DEE9;
    default_bg = SEE_THROUGH | (settings.light ? 0xFAFAFA : 0x161A22);
}

// --- Drawing ---

// A line counting from the oldest kept, through the scrollback and onto the screen.
static const struct cell *line_cells(int line) {
    return line < t->history_count ? history_line(line) : t->cells[line - t->history_count];
}

// What's shown on row r of the window, which is in the scrollback when scrolled back.
static int shown_line(int r) {
    return t->history_count - t->view + r;
}

static long order(struct place p) {
    return (long)p.line * MAX_COLS + p.col;
}

static bool is_selected(int line, int c) {
    long from = order(t->select_from), to = order(t->select_to), at = order((struct place){line, c});

    return t->selected && at >= (from < to ? from : to) && at <= (from < to ? to : from);
}

static void draw_cell(int r, int c) {
    const struct cell *line = line_cells(shown_line(r));
    struct cell cell = line[c];
    uint32_t ch = cell.ch & CH_MASK;
    bool wide = cell.ch & ATTR_WIDE, bold = cell.ch & ATTR_BOLD;
    bool cursor = r - t->view == t->row && c == t->col && !t->cursor_hidden && !t->view;
    uint32_t fg = resolve(cell.fg, true, bold), bg = resolve(cell.bg, false, false);
    int x = left + c * CELL_WIDTH, y = top + r * CELL_HEIGHT, width = (wide && c + 1 < cols ? 2 : 1) * CELL_WIDTH;

    if (!ch && c > 0 && line[c - 1].ch & ATTR_WIDE)
        return; // the second half of a wide character, drawn with the first
    if (cell.ch & ATTR_REVERSE) {
        uint32_t swap = fg;

        fg = bg & 0xFFFFFF, bg = swap;
    }
    if (cell.bg != COLOR_DEFAULT || cell.ch & ATTR_REVERSE)
        bg &= 0xFFFFFF; // only the default background is see-through
    // A solid block when focused, an outline when not, and what's selected reversed.
    if ((cursor && focused) != is_selected(shown_line(r), c)) {
        uint32_t swap = fg;

        fg = bg & 0xFFFFFF, bg = swap;
    }
    if (cell.ch & ATTR_DIM)
        fg = gui_mix(fg, bg, 110);
    gui_fill(&window->canvas, x, y, width, CELL_HEIGHT, bg);
    if (!(cell.ch & ATTR_HIDDEN))
        gui_cell_char(&window->canvas, x, y, ch, width / CELL_WIDTH, bold, fg);
    if (cell.ch & ATTR_UNDERLINE)
        gui_fill(&window->canvas, x, y + CELL_HEIGHT - 2, width, 1, fg);
    if (cell.ch & ATTR_STRIKE)
        gui_fill(&window->canvas, x, y + CELL_HEIGHT / 2, width, 1, fg);
    if (cursor && !focused) {
        gui_fill(&window->canvas, x, y, width, 1, fg);
        gui_fill(&window->canvas, x, y + CELL_HEIGHT - 1, width, 1, fg);
        gui_fill(&window->canvas, x, y, 1, CELL_HEIGHT, fg);
        gui_fill(&window->canvas, x + width - 1, y, 1, CELL_HEIGHT, fg);
    }
}

// A thumb in the right margin while scrolled back, showing where.
static void draw_scrollbar(void) {
    int x = area.x + area.width - PADDING, height = area.height, total = t->history_count + rows;

    gui_fill(&window->canvas, x, area.y, PADDING, height, default_bg);
    if (t->view)
        gui_fill(&window->canvas, x + 1, area.y + (t->history_count - t->view) * height / total, PADDING - 2,
                 rows * height / total, colors[8]);
    gui_update(window, x, area.y, PADDING, height);
}

// Whether libgui's showing something over the cells: a menu, or the help.
static bool over_cells(void) {
    return ui.popup || ui.help_open || ui.dialog_shown;
}

// The cells that changed, or all of the window, libgui's widgets and all, when
// they've changed, or there's something of theirs over the cells.
static void frame(void);

static void draw(void) {
    static int drawn_view;

    if (stale || over_cells()) {
        frame();
        return;
    }
    int first = rows, last = -1;

    // Changes on the screen move about the window while scrolled back.
    for (int r = 0; t->view && r < rows; r++) {
        if (dirty[r])
            mark_all_dirty();
    }
    if (t->view != drawn_view || (t->view && dirty[0]))
        draw_scrollbar();
    drawn_view = t->view;
    for (int r = 0; r < rows; r++) {
        if (!dirty[r])
            continue;
        for (int c = 0; c < cols; c++)
            draw_cell(r, c);
        dirty[r] = false;
        first = r < first ? r : first;
        last = r;
    }
    if (last >= 0)
        gui_update(window, left, top + first * CELL_HEIGHT, cols * CELL_WIDTH, (last - first + 1) * CELL_HEIGHT);
}

void vt_title_changed(void) {
    if (window && t == sessions[active])
        gui_set_title(window, t->title[0] ? t->title : "Terminal");
    stale |= session_count > 1; // the tab's name
}

// For the tests: the active tab's screen, its text a row a line, and where the cursor is.
static void dump_screen(void) {
    printf("term: screen %dx%d cursor %d %d%s\n", cols, rows, t->row, t->col, t->alternate_screen ? " alternate" : "");
    for (int r = 0; r < rows; r++) {
        char line[MAX_COLS * 4 + 1];
        size_t n = 0, end = 0;

        for (int c = 0; c < cols; c++) {
            uint32_t ch = t->cells[r][c].ch & CH_MASK;

            if (!ch)
                continue; // the second half of a wide character
            n += utf8(ch, line + n);
            if (ch != ' ')
                end = n;
        }
        line[end] = '\0';
        printf("term: row %d %s\n", r, line);
    }
    fflush(stdout);
}

// --- Keys, the mouse and pasting, to the program ---

// xterm's modifier number: 1, and 1 for Shift, 2 for Alt and 4 for Ctrl.
static int modifier_number(uint8_t m) {
    return 1 + (m & KEY_MOD_SHIFT ? 1 : 0) + (m & KEY_MOD_ALT ? 2 : 0) + (m & KEY_MOD_CTRL ? 4 : 0);
}

// What a key sends, as xterm sends it. Alt adds an escape first. Returns whether it sent anything.
static bool send_key(int master, const struct key_event *key) {
    static const struct { uint16_t code; char final; int number; } special[] = {
        {KEY_UP, 'A', 0}, {KEY_DOWN, 'B', 0}, {KEY_RIGHT, 'C', 0}, {KEY_LEFT, 'D', 0}, {KEY_HOME, 'H', 0},
        {KEY_END, 'F', 0}, {KEY_INSERT, '~', 2}, {KEY_DELETE, '~', 3}, {KEY_PAGE_UP, '~', 5}, {KEY_PAGE_DOWN, '~', 6},
        {KEY_F1, 'P', 0}, {KEY_F1 + 1, 'Q', 0}, {KEY_F1 + 2, 'R', 0}, {KEY_F1 + 3, 'S', 0}, {KEY_F1 + 4, '~', 15},
        {KEY_F1 + 5, '~', 17}, {KEY_F1 + 6, '~', 18}, {KEY_F1 + 7, '~', 19}, {KEY_F1 + 8, '~', 20}, {KEY_F10, '~', 21},
        {KEY_F11, '~', 23}, {KEY_F12, '~', 24},
    };
    int m = modifier_number(key->modifiers);
    char out[32] = "";

    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); i++) {
        if (key->code != special[i].code)
            continue;

        bool ss3 = special[i].final >= 'P' && special[i].final <= 'S';

        if (special[i].number)
            snprintf(out, sizeof(out), m > 1 ? "\x1b[%d;%d~" : "\x1b[%d~", special[i].number, m);
        else if (m > 1)
            snprintf(out, sizeof(out), "\x1b[1;%d%c", m, special[i].final);
        else
            snprintf(out, sizeof(out), "\x1b%c%c", ss3 || (t->app_cursor && special[i].final <= 'H') ? 'O' : '[',
                     special[i].final);
        write_all(master, out, strlen(out));
        return true;
    }

    char text = (char)key->text;

    if (key->code == KEY_TAB && key->modifiers & KEY_MOD_SHIFT)
        snprintf(out, sizeof(out), "\x1b[Z");
    else if (key->code == KEY_BACKSPACE)
        snprintf(out, sizeof(out), "%s%c", key->modifiers & KEY_MOD_ALT ? "\x1b" : "", key->modifiers & KEY_MOD_CTRL ? 0x08 : 0x7F);
    else if (key->code == KEY_SPACE && key->modifiers & KEY_MOD_CTRL)
        write_all(master, "", 1); // NUL
    else if (text)
        snprintf(out, sizeof(out), "%s%c", key->modifiers & KEY_MOD_ALT ? "\x1b" : "", text);
    write_all(master, out, strlen(out));
    return out[0] || (key->code == KEY_SPACE && key->modifiers & KEY_MOD_CTRL);
}

// A mouse report, in SGR's form if the program asked, else the old one.
static void report_mouse(int button, int col, int row, bool release, bool motion) {
    char out[32];
    int b = button | (motion ? 32 : 0) | (modifiers & KEY_MOD_SHIFT ? 4 : 0) | (modifiers & KEY_MOD_ALT ? 8 : 0)
          | (modifiers & KEY_MOD_CTRL ? 16 : 0);

    if (t->mouse_sgr)
        snprintf(out, sizeof(out), "\x1b[<%d;%d;%d%c", b, col + 1, row + 1, release ? 'm' : 'M');
    else
        snprintf(out, sizeof(out), "\x1b[M%c%c%c", 32 + (release ? 3 : b), 32 + clamp(col + 1, 1, 223),
                 32 + clamp(row + 1, 1, 223));
    write_all(t->master, out, strlen(out));
}

// Whether the mouse goes to the program, which asks for it, unless Shift's held to select.
static bool program_has_mouse(const struct gui_message *e) {
    if (!t->mouse || modifiers & KEY_MOD_SHIFT || e->y < top || t->view)
        return false;

    int col = clamp((e->x - left) / CELL_WIDTH, 0, cols - 1), row = clamp((e->y - top) / CELL_HEIGHT, 0, rows - 1);

    if (e->type == GUI_MOUSE_BUTTON) {
        for (int b = 0; b < 3; b++) {
            static const int numbers[3] = {0, 2, 1}; // left, right, middle as gui has them, to xterm's
            if (e->changed & 1u << b) {
                bool down = e->buttons & 1u << b;

                mouse_button = down ? numbers[b] : -1;
                report_mouse(numbers[b], col, row, !down, false);
            }
        }
    } else if (e->type == GUI_MOUSE_MOVE && (t->mouse == 1003 || (t->mouse == 1002 && mouse_button >= 0))) {
        report_mouse(mouse_button >= 0 ? mouse_button : 3, col, row, false, true);
    } else if (e->type == GUI_MOUSE_WHEEL) {
        for (int i = 0; i < (e->wheel < 0 ? -e->wheel : e->wheel); i++)
            report_mouse(e->wheel < 0 ? 64 : 65, col, row, false, false);
    }
    return true;
}

static void scroll_view(int lines) {
    t->view = clamp(t->view + lines, 0, t->history_count);
    mark_all_dirty();
}

// Pasting types what's on the clipboard, with Enter for each line break, marked as
// pasted if the program asked.
static void paste(int master) {
    size_t length;
    char *text = gui_clipboard_get(&length);

    if (!text)
        return;
    for (size_t i = 0; i < length; i++)
        text[i] = text[i] == '\n' ? '\r' : text[i];
    if (t->bracketed_paste)
        write_all(master, "\x1b[200~", 6);
    write_all(master, text, length);
    if (t->bracketed_paste)
        write_all(master, "\x1b[201~", 6);
    free(text);
}

// Pastes into the program, taking the view back down to it.
static void paste_here(void) {
    if (t->view)
        scroll_view(-t->view);
    paste(t->master);
}

// A file dropped on the window types its path, quoted for the shell, and a space,
// as if it were pasted.
static void drop_path(int master, const char *path) {
    bool plain = path[strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-+,:@%")] == '\0';

    if (t->bracketed_paste)
        write_all(master, "\x1b[200~", 6);
    if (plain) {
        write_all(master, path, strlen(path));
    } else {
        write_all(master, "'", 1);
        for (const char *at = path; *at; at++)
            *at == '\'' ? write_all(master, "'\\''", 4) : write_all(master, at, 1);
        write_all(master, "'", 1);
    }
    write_all(master, " ", 1);
    if (t->bracketed_paste)
        write_all(master, "\x1b[201~", 6);
    printf("term: dropped %s\n", path);
    fflush(stdout);
}

// --- Selecting ---

// The cell under a point in the window.
static struct place place_at(int x, int y) {
    return (struct place){shown_line(clamp((y - top) / CELL_HEIGHT, 0, rows - 1)), clamp((x - left) / CELL_WIDTH, 0, cols - 1)};
}

static bool is_space(const struct cell *c) {
    return (c->ch & CH_MASK) == ' ';
}

// Selects the word at a place: the run of characters there that aren't spaces.
static void select_word(struct place p) {
    const struct cell *line = line_cells(p.line);
    int from = p.col, to = p.col;

    while (from > 0 && !is_space(&line[from - 1]))
        from--;
    while (to < cols - 1 && !is_space(&line[to + 1]))
        to++;
    t->select_from = (struct place){p.line, from};
    t->select_to = (struct place){p.line, to};
    t->selected = !is_space(&line[p.col]);
}

// The mouse selects by dragging, or a word with a double click. A click on its own
// selects nothing.
static void mouse(const struct gui_message *e) {
    if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
        selecting = e->buttons & 1;
        if (selecting && now_ms() - last_press < 400) {
            select_word(place_at(e->x, e->y));
            selecting = false;
            last_press = 0;
        } else if (selecting) {
            t->select_from = t->select_to = place_at(e->x, e->y);
            t->selected = false;
            last_press = now_ms();
        }
    } else if (e->type == GUI_MOUSE_MOVE && selecting) {
        t->select_to = place_at(e->x, e->y);
        t->selected = order(t->select_from) != order(t->select_to);
    }
    drag_x = e->x;
    drag_y = e->y;
    mark_all_dirty();
}

// How many lines back to scroll while a selection is dragged above the window,
// or forward when below it, more the further out it is.
static int drag_lines(void) {
    int bottom = top + rows * CELL_HEIGHT;

    if (!selecting)
        return 0;
    if (drag_y < top)
        return 1 + (top - drag_y) / CELL_HEIGHT;
    if (drag_y >= bottom)
        return -(1 + (drag_y - bottom) / CELL_HEIGHT);
    return 0;
}

// Scrolls every DRAG_SCROLL_MS while the selection is dragged out of the window,
// taking the selection along, and returns how long until the next scroll, or -1.
static int drag_scroll(void) {
    static long last;
    long now = now_ms();
    int lines = drag_lines();

    if (!lines)
        return -1;
    if (now - last >= DRAG_SCROLL_MS) {
        last = now;
        scroll_view(lines);
        t->select_to = place_at(drag_x, drag_y);
        t->selected = order(t->select_from) != order(t->select_to);
    }
    return (int)(DRAG_SCROLL_MS - (now - last));
}

// Each line's text, without the spaces at its end, with a line break between them.
static void copy_selection(void) {
    struct place from = order(t->select_from) < order(t->select_to) ? t->select_from : t->select_to;
    struct place to = order(t->select_from) < order(t->select_to) ? t->select_to : t->select_from;
    char *text = t->selected ? malloc((size_t)(to.line - from.line + 1) * (MAX_COLS * 4 + 1)) : NULL;
    size_t length = 0;

    if (!text)
        return;
    for (int line = from.line; line <= to.line; line++) {
        const struct cell *cells_of = line_cells(line);
        int first = line == from.line ? from.col : 0, last = line == to.line ? to.col : cols - 1;

        while (last >= first && is_space(&cells_of[last]))
            last--;
        for (int c = first; c <= last; c++) {
            if (cells_of[c].ch & CH_MASK)
                length += utf8(cells_of[c].ch & CH_MASK, text + length);
        }
        if (line < to.line)
            text[length++] = '\n';
    }
    gui_clipboard_set(text, length);
    free(text);
}

// --- Tabs and the window ---


// How much of a line has text: all of it if it goes on in the next, or else up to
// its last cell that isn't blank.
static int text_length(const struct cell *line) {
    int n = cols;

    if (line_wraps(line))
        return n;
    while (n > 0 && (line[n - 1].ch & (CH_MASK | ATTR_REVERSE)) == ' ' && line[n - 1].bg == COLOR_DEFAULT)
        n--;
    return n;
}

// The lines kept and on the primary screen wrapped again at a new width: each that
// wrapped joined to the next, and what they make split again where it no longer fits,
// a wide character going whole to the next line. The newest go on the screen, with
// the cursor where it was in its text, and those before back in what's kept.
#define REFLOWED (SCROLLBACK + MAX_ROWS) // the newest kept as they're made
static struct cell *made(struct cell (*out)[MAX_COLS], long i) {
    return out[i % REFLOWED];
}

// The next line made, blank, which the one before goes on in if it wraps.
static void next_line(struct cell (*out)[MAX_COLS], long *n, int *x, bool wraps) {
    if (wraps)
        made(out, *n)[MAX_COLS - 1].ch |= LINE_WRAPS;
    struct cell *line = made(out, ++*n);

    *x = 0;
    for (int c = 0; c < MAX_COLS; c++)
        line[c] = (struct cell){' ', COLOR_DEFAULT, COLOR_DEFAULT};
}

static bool reflow(struct session *s, int new_cols, int new_rows) {
    struct cell(*out)[MAX_COLS] = malloc(REFLOWED * sizeof(*out));
    const struct cell blank = {' ', COLOR_DEFAULT, COLOR_DEFAULT};
    long n = -1, cursor_line = 0;
    int x = 0, cursor_col = 0, last = s->row;

    if (!out)
        return false;
    for (int r = rows - 1; r > s->row; r--) {
        if (text_length(s->primary[r])) {
            last = r;
            break;
        }
    }

    int lines = s->history_count + last + 1;

    next_line(out, &n, &x, false);
    for (int i = 0; i < lines; i++) {
        const struct cell *line = i < s->history_count ? history_line(i) : s->primary[i - s->history_count];
        int length = text_length(line);
        bool cursor = i == s->history_count + s->row;
        int at = s->col + (s->wrap_next ? 1 : 0); // the cursor's cell, past the last if it's waiting to wrap

        for (int c = 0; c < length; c++) {
            int width = line[c].ch & ATTR_WIDE ? 2 : 1;

            if (!(line[c].ch & CH_MASK) && c > 0 && line[c - 1].ch & ATTR_WIDE)
                continue; // the second half of a wide character, which goes with the first
            if (x + width > new_cols)
                next_line(out, &n, &x, true);
            if (cursor && (c == at || (width == 2 && c + 1 == at)))
                cursor_line = n, cursor_col = x;
            made(out, n)[x] = line[c];
            if (width == 2)
                made(out, n)[x + 1] = line[c + 1];
            x += width;
        }
        if (cursor && at >= length) { // past the text, as after a prompt's space
            int to = x + at - length;

            for (; to >= new_cols; to -= new_cols)
                next_line(out, &n, &x, true);
            cursor_line = n, cursor_col = to;
        }
        if (!line_wraps(line) || i == lines - 1)
            next_line(out, &n, &x, false);
    }

    // n lines made, and the blank one after them, the newest REFLOWED of those kept: the
    // screen's are the newest, with the cursor's among them.
    long first = n >= REFLOWED ? n - REFLOWED + 1 : 0, start = n - new_rows;

    start = start > cursor_line ? cursor_line : start < first ? first : start;
    long kept_from = start - SCROLLBACK > first ? start - SCROLLBACK : first;

    s->history_start = 0;
    s->history_count = (int)(start - kept_from);
    for (long i = kept_from; i < start; i++)
        memcpy(s->history[i - kept_from], made(out, i), sizeof(s->history[0]));
    for (int r = 0; r < new_rows; r++) {
        if (start + r < n)
            memcpy(s->primary[r], made(out, start + r), sizeof(s->primary[0]));
        else
            for (int c = 0; c < MAX_COLS; c++)
                s->primary[r][c] = blank;
        for (int c = 0; c < MAX_COLS; c++)
            s->alternate[r][c] = blank; // not showing, and drawn afresh when it's next shown
    }
    s->row = (int)(cursor_line - start);
    s->col = cursor_col < new_cols ? cursor_col : new_cols - 1;
    s->selected = false;
    s->view = 0;
    free(out);
    return true;
}

// Fits the terminal to a new size, and tells the programs on it. The primary screen's
// wrapped again, unless a full-screen program's on the alternate one, which it draws
// again itself, and as many cells as fit are kept, with the cursor's line on the screen.
static void fit(struct session *s, int new_cols, int new_rows) {
    struct session *was = t;
    int lost = s->row - (new_rows - 1);

    t = s;
    if (!s->alternate_screen && reflow(s, new_cols, new_rows)) {
        lost = 0;
        goto fitted;
    }
    if (lost > 0) {
        for (int r = 0; r < lost && !s->alternate_screen; r++)
            keep_line(s->primary[r]);
        memmove(s->primary[0], s->primary[lost], (rows - lost) * sizeof(s->primary[0]));
        memmove(s->alternate[0], s->alternate[lost], (rows - lost) * sizeof(s->alternate[0]));
        s->row -= lost;
    }

    // What comes into view is blank.
    int kept = lost > 0 ? rows - lost : rows;

    for (int r = 0; r < new_rows; r++) {
        for (int c = r < kept ? cols : 0; c < new_cols; c++)
            s->primary[r][c] = s->alternate[r][c] = (struct cell){' ', COLOR_DEFAULT, COLOR_DEFAULT};
    }
fitted:
    s->col = clamp(s->col, 0, new_cols - 1);
    s->view = clamp(s->view, 0, s->history_count);
    s->top = 0;
    s->bottom = new_rows - 1;
    s->wrap_next = false;

    struct winsize size = {.ws_row = new_rows, .ws_col = new_cols, .ws_xpixel = new_cols * CELL_WIDTH,
                           .ws_ypixel = new_rows * CELL_HEIGHT};

    ioctl(s->master, TIOCSWINSZ, &size);
    t = was;
}

// Fits every terminal to the area the cells have, under the tabs and over the find bar.
static void fit_area(void) {
    int new_cols = clamp((area.width - 2 * PADDING) / CELL_WIDTH, 1, MAX_COLS - 1); // the last's for LINE_WRAPS
    int new_rows = clamp((area.height - 2 * PADDING) / CELL_HEIGHT, 1, MAX_ROWS);

    left = area.x + PADDING, top = area.y + PADDING;
    if (new_cols != cols || new_rows != rows) {
        for (int i = 0; i < session_count; i++)
            fit(sessions[i], new_cols, new_rows);
        cols = new_cols;
        rows = new_rows;
    }
    t = sessions[active];
}

static bool new_session(char **command) {
    struct winsize size = {.ws_row = rows, .ws_col = cols, .ws_xpixel = cols * CELL_WIDTH, .ws_ypixel = rows * CELL_HEIGHT};
    static int opened;
    struct session *s;
    int slave;

    if (session_count == MAX_TABS || !(s = calloc(1, sizeof(*s))))
        return false;
    if (openpty(&s->master, &slave, NULL, NULL, &size) < 0) {
        free(s);
        return false;
    }
    s->child = fork();
    if (s->child == 0) {
        close(s->master);
        close(gui_fd());
        for (int i = 0; i < session_count; i++)
            close(sessions[i]->master);
        login_tty(slave);
        setenv("TERM", "xterm-256color", 1); // what it is, which programs find in terminfo
        setenv("COLORTERM", "truecolor", 1);
        if (command && *command)
            execvp(command[0], command);
        else { // the account's shell, as a login shell, so it reads /etc/profile
            const char *shell = getenv("SHELL") && *getenv("SHELL") ? getenv("SHELL") : "/bin/sh";
            char login[64];

            snprintf(login, sizeof(login), "-%s", strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell);
            execl(shell, login, (char *)NULL);
        }
        _exit(127);
    }
    close(slave);
    s->number = ++opened;
    vt_init(s);
    sessions[session_count++] = s;
    active = session_count - 1;
    t = s;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    stale = true;
    return true;
}

static void switch_to(int index) {
    active = (index + session_count) % session_count;
    t = sessions[active];
    searching = false;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    vt_title_changed();
    stale = true;
}

// Closing the master hangs up whatever is running on the terminal.
static void close_session(int index) {
    close(sessions[index]->master);
    kill(sessions[index]->child, SIGHUP);
    free(sessions[index]);
    memmove(sessions + index, sessions + index + 1, (session_count - index - 1) * sizeof(*sessions));
    session_count--;
    if (!session_count)
        return;
    active = active > index || active == session_count ? active - 1 : active;
    printf("term: tab %d of %d\n", active + 1, session_count);
    fflush(stdout);
    t = sessions[active];
    vt_title_changed();
    stale = true;
}

// Searches what's kept for the query, from the line before or after the last found,
// going older or newer, and shows it selected. Only ASCII is matched without case.
static void find(bool older) {
    int total = t->history_count + rows, step = older ? -1 : 1;
    int from = found_line >= 0 && found_line < total ? found_line + step : older ? total - 1 : 0;
    size_t length = strlen(query);

    for (int line = from; length && line >= 0 && line < total; line += step) {
        const struct cell *cells_of = line_cells(line);

        for (int c = 0; c + (int)length <= cols; c++) {
            size_t i = 0;

            while (i < length && (cells_of[c + i].ch & CH_MASK) < 0x80
                   && tolower((int)(cells_of[c + i].ch & CH_MASK)) == tolower((unsigned char)query[i]))
                i++;
            if (i < length)
                continue;
            found_line = line;
            t->select_from = (struct place){line, c};
            t->select_to = (struct place){line, c + (int)length - 1};
            t->selected = true;
            t->view = clamp(t->history_count + rows / 2 - line, 0, t->history_count);
            snprintf(search_status, sizeof(search_status), "line %d", line + 1);
            printf("term: found %s on line %d\n", query, line + 1);
            fflush(stdout);
            mark_all_dirty();
            draw_scrollbar();
            return;
        }
    }
    snprintf(search_status, sizeof(search_status), length ? "not found" : "");
    printf("term: %s not found\n", query);
    fflush(stdout);
}

static void start_search(void) {
    searching = search_started = stale = true;
    query[0] = search_status[0] = '\0';
    found_line = -1;
}

static void stop_search(void) {
    searching = false;
    stale = true;
}

// The window, as libgui draws it: the tabs, when there's more than one, the cells
// under them, the find bar under those while searching, and over them, the menu a
// right-click opens and the help. Each terminal's fitted to the cells' area.
static void frame(void) {
    struct gui_canvas *c = &window->canvas;
    const char *names[MAX_TABS];
    char named[MAX_TABS][32];
    int chosen = active, closing = -1;

    stale = false;
    gui_ui_begin(&ui);
    gui_help(&ui, help);
    if (session_count > 1) {
        for (int i = 0; i < session_count; i++) {
            snprintf(named[i], sizeof(named[i]), "%s", sessions[i]->title);
            if (!named[i][0])
                snprintf(named[i], sizeof(named[i]), "Shell %d", sessions[i]->number);
            names[i] = named[i];
        }

        int r = gui_tabs(&ui, names, session_count, &chosen, &closing);

        if (r == GUI_TABS_CHANGED)
            switch_to(chosen);
        else if (r == GUI_TABS_CLOSE)
            close_session(closing);
        else if (r == GUI_TABS_NEW)
            new_session(NULL);
        if (!session_count)
            return;
    }

    // The cells, from under the tabs to the bottom, or the find bar.
    int find_bar = searching ? FIND_BAR : 0, y = session_count > 1 ? ui.layout.y - WIDGET_MARGIN : 0;

    area = (struct gui_rect){0, y, c->width, c->height - y - find_bar};
    fit_area();
    gui_fill(c, area.x, area.y, area.width, area.height, default_bg);
    mark_all_dirty();
    for (int r = 0; r < rows; r++) {
        for (int col = 0; col < cols; col++)
            draw_cell(r, col);
        dirty[r] = false;
    }
    draw_scrollbar();

    // Finding: Enter finds older, Shift+Enter newer, and Escape stops.
    if (searching) {
        ui.layout.y = area.y + area.height + WIDGET_MARGIN;
        ui.layout.x = ui.layout.left;
        if (search_started)
            gui_set_focus(&ui, FIND, NULL);
        search_started = false;
        gui_column_begin(&ui, c->width - FIND_BUTTONS); // the box, and the buttons beside it
        if (gui_textbox(&ui, FIND, query, sizeof(query)))
            found_line = -1;
        gui_column_end(&ui);
        if (gui_button(&ui, "Older") || (gui_key(&ui, KEY_ENTER) && !(modifiers & KEY_MOD_SHIFT)))
            find(true);
        gui_same_line(&ui);
        if (gui_button(&ui, "Newer") || (gui_key(&ui, KEY_ENTER) && modifiers & KEY_MOD_SHIFT))
            find(false);
        gui_same_line(&ui);
        gui_label(&ui, "%s", search_status);
        gui_same_line(&ui);
        if (gui_button(&ui, "Close") || gui_key(&ui, KEY_ESCAPE))
            stop_search();
    }

    if (gui_context_menu(&ui, "##menu", area)) {
        if (gui_menu_icon_item(&ui, "Copy", gui_icon("copy")))
            copy_selection();
        if (gui_menu_icon_item(&ui, "Paste", gui_icon("clipboard")))
            paste_here();
        gui_menu_separator(&ui);
        if (gui_menu_icon_item(&ui, "New tab", gui_icon("add")))
            new_session(NULL);
        if (gui_menu_icon_item(&ui, "Find...", gui_icon("find")))
            start_search();
        if (gui_menu_icon_item(&ui, "Keyboard shortcuts", gui_icon("keyboard")))
            gui_help_open(&ui);
        gui_menu_end(&ui);
    }
    if (!over_cells() && ui.mouse_y >= area.y && ui.mouse_y < area.y + area.height)
        ui.cursor = GUI_CURSOR_TEXT;
    gui_ui_end(&ui);
}

// Whether an event's libgui's: the mouse over its widgets, or anywhere while it's
// showing something over the cells, a right-click on them, which opens the menu,
// and the keys while the find bar or something over the cells has them.
static bool for_widgets(const struct gui_message *e) {
    bool mouse = e->type == GUI_MOUSE_MOVE || e->type == GUI_MOUSE_BUTTON || e->type == GUI_MOUSE_WHEEL;
    bool outside = e->y < area.y || e->y >= area.y + area.height;
    bool right = e->type == GUI_MOUSE_BUTTON && (e->changed & 2) && (e->buttons & 2);

    if (mouse && !selecting) {
        bool was = mouse_on_widgets;

        mouse_on_widgets = outside || over_cells();
        return mouse_on_widgets || was || (right && !program_has_mouse(e));
    }
    if (e->type == GUI_KEY) // Ctrl+Q is the program's, which libgui would quit for
        return (over_cells() || (searching && gui_focused(&ui, FIND)) || e->key.code == KEY_F1)
               && !((e->key.modifiers & KEY_MOD_CTRL) && e->key.code == KEY_Q);
    return e->type == GUI_TIMER || e->type == GUI_REDRAW;
}

// Keys for the window rather than the program: copying and pasting, tabs and finding.
static bool window_key(const struct key_event *k) {
    bool shifted = k->modifiers & KEY_MOD_SHIFT, ctrl = k->modifiers & KEY_MOD_CTRL;

    if (ctrl && shifted && k->code == KEY_C)
        copy_selection();
    else if (ctrl && shifted && k->code == KEY_V)
        paste_here();
    else if (ctrl && shifted && k->code == KEY_T)
        new_session(NULL);
    else if (ctrl && shifted && k->code == KEY_W)
        close_session(active);
    else if (ctrl && shifted && k->code == KEY_F)
        start_search();
    else if (ctrl && (k->code == KEY_TAB || k->code == KEY_PAGE_DOWN || k->code == KEY_PAGE_UP))
        switch_to(active + (k->code == KEY_PAGE_UP || (k->code == KEY_TAB && shifted) ? -1 : 1));
    else if (shifted && !t->alternate_screen && (k->code == KEY_PAGE_UP || k->code == KEY_PAGE_DOWN))
        scroll_view((k->code == KEY_PAGE_UP ? 1 : -1) * (rows > 1 ? rows - 1 : 1));
    else
        return false;
    return true;
}

static void dump_signal(int signal) {
    (void)signal;
    dump_asked = 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: term [command [argument]...]\n"
                            "Opens a terminal window running the shell, or the command. It's an xterm to\n"
                            "programs, TERM=xterm-256color. SIGUSR1 has it print its screen, for tests.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "term: wm isn't running\n");
        return 1;
    }
    sigset_t blocked, waiting;

    make_colors();
    use_theme();
    // SIGUSR1 comes only while waiting, so asking can't slip in after it's looked for.
    signal(SIGUSR1, dump_signal);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blocked, &waiting);
    if (!new_session(argv + 1)) {
        dprintf(STDERR_FILENO, "term: %s\n", strerror(errno));
        return 1;
    }
    window = gui_create_window("Terminal", cols * CELL_WIDTH + 2 * PADDING, rows * CELL_HEIGHT + 2 * PADDING);
    if (!window) {
        dprintf(STDERR_FILENO, "term: couldn't make a window\n");
        return 1;
    }
    gui_set_cursor(window, GUI_CURSOR_TEXT);
    gui_set_translucent(window, true);
    gui_ui_init(&ui, window);
    frame();

    while (session_count) {
        struct pollfd fds[1 + MAX_TABS] = {{.fd = gui_fd(), .events = POLLIN}};
        char buf[16384];
        int count = session_count;

        for (int i = 0; i < count; i++)
            fds[1 + i] = (struct pollfd){.fd = sessions[i]->master, .events = POLLIN};

        int timeout = drag_scroll(), due = gui_due_ms();

        // What's come already goes at once, and libgui's timers and repeating keys when they're due.
        timeout = gui_ready() ? 0 : due >= 0 && (timeout < 0 || due < timeout) ? due : timeout;

        draw(); // what came last time round, and any scrolling
        if (dump_asked) {
            dump_asked = 0;
            dump_screen();
        }
        struct timespec wait = {timeout / 1000, timeout % 1000 * 1000000L};

        if (ppoll(fds, 1 + count, timeout < 0 ? NULL : &wait, &waiting) < 0 && errno != EINTR)
            break;
        if (dump_asked)
            continue; // to print it, at the top


        // Each program's output, and its tab closing when everything on its terminal has exited.
        for (int i = count - 1; i >= 0; i--) {
            if (!(fds[1 + i].revents & (POLLIN | POLLHUP)))
                continue;

            ssize_t n = read(sessions[i]->master, buf, sizeof(buf));

            t = sessions[i];
            dirty[t->row] = i == active; // where the cursor was
            if (n > 0)
                vt_write(buf, (size_t)n);
            if (i == active)
                dirty[t->row] = true; // where the cursor is now
            t = sessions[active];
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                close_session(i);
                if (!session_count)
                    break;
            }
        }
        if (!session_count)
            break;

        struct gui_message e;

        int got = gui_wait_event(&e, 0);

        if (got == 1 && for_widgets(&e)) {
            if (e.type == GUI_KEY)
                modifiers = e.key.modifiers;
            stale |= gui_ui_event(&ui, &e);
        } else if (got == 1) {
            // Keys go to the program, taking the view back to the bottom, unless
            // they're the window's.
            if (e.type == GUI_KEY) {
                modifiers = e.key.modifiers;
                if (e.key.pressed && !window_key(&e.key) && send_key(t->master, &e.key) && t->view)
                    scroll_view(-t->view);
            } else if (e.type == GUI_MOUSE_MOVE || e.type == GUI_MOUSE_BUTTON) {
                if (!program_has_mouse(&e))
                    mouse(&e);
            } else if (e.type == GUI_MOUSE_WHEEL) {
                if (program_has_mouse(&e))
                    ;
                else if (t->alternate_screen) // a full-screen program's arrow keys, as xterm does
                    for (int i = 0; i < WHEEL_LINES * (e.wheel < 0 ? -e.wheel : e.wheel); i++)
                        write_all(t->master, e.wheel < 0 ? (t->app_cursor ? "\x1bOA" : "\x1b[A") : (t->app_cursor ? "\x1bOB" : "\x1b[B"), 3);
                else
                    scroll_view(-e.wheel * WHEEL_LINES);
            } else if (e.type == GUI_FOCUS) {
                gui_ui_event(&ui, &e);
                stale |= over_cells() || searching;
                focused = e.focused;
                dirty[t->row] = true;
                if (t->focus_events)
                    write_all(t->master, focused ? "\x1b[I" : "\x1b[O", 3);
            } else if (e.type == GUI_DROP) {
                char path[GUI_PATH_MAX];

                if (gui_dropped_file(&e, path))
                    drop_path(t->master, path);
            } else if (e.type == GUI_RESIZE) {
                stale |= gui_ui_event(&ui, &e);
            } else if (e.type == GUI_SETTINGS) {
                gui_ui_event(&ui, &e);
                use_theme();
                stale = true;
            } else if (e.type == GUI_CLOSE) {
                break;
            }
        } else if (got < 0) {
            break; // the compositor has gone
        }
    }
    while (session_count)
        close_session(session_count - 1);
    gui_destroy_window(window);
    return 0;
}
