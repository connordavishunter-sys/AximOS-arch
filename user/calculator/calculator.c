#include <aximos.h>
#include <gui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DISPLAY_HEIGHT 56
#define BUTTON_HEIGHT 40
#define GAP 8 // between buttons, as libgui puts them
#define DIGITS 15
#define PAD 240     // wide, the display and the buttons
#define HISTORY 64  // sums kept, newest first
#define HISTORY_WIDTH 220

// The buttons, four to a row, with 0 twice as wide.
static const char *const keys[] = { // clang-format off
    "C", "+/-", "%", "/",
    "7", "8",   "9", "*",
    "4", "5",   "6", "-",
    "1", "2",   "3", "+",
    "0", ".",   "=",
}; // clang-format on
#define KEY_COUNT ((int)(sizeof(keys) / sizeof(keys[0])))

// What's being typed, and what's waiting for it: the number before, and its operator.
static char entry[DIGITS + 3] = "0";
static double before;
static char op;
static bool fresh = true; // the next digit starts a new number
static bool error;

// The sums done, newest first, and the answer of each.
static char history[HISTORY][64];
static const char *history_items[HISTORY];
static double answers[HISTORY];
static int history_count, history_selected = -1;

static void remember(double a, char o, double b, double result) {
    char line[64];

    snprintf(line, sizeof(line), "%.10g %c %.10g = %.10g", a, o, b, result);
    memmove(history[1], history[0], (HISTORY - 1) * sizeof(history[0]));
    memmove(answers + 1, answers, (HISTORY - 1) * sizeof(*answers));
    snprintf(history[0], sizeof(history[0]), "%s", line);
    answers[0] = result;
    history_count += history_count < HISTORY;
    for (int i = 0; i < history_count; i++)
        history_items[i] = history[i];
    history_selected = -1;
}

static double value(void) {
    return strtod(entry, NULL);
}

static void show(double v) {
    error = !isfinite(v);
    snprintf(entry, sizeof(entry), "%.12g", v == 0 ? 0 : v); // never -0
    if (strlen(entry) > DIGITS)
        snprintf(entry, sizeof(entry), "%.8g", v);
}

static double apply(double a, char o, double b) {
    return o == '+' ? a + b : o == '-' ? a - b : o == '*' ? a * b : o == '/' ? a / b : b;
}

static void press(const char *key) {
    char c = key[0];

    if (error && c != 'C')
        return;
    if (c >= '0' && c <= '9') {
        if (fresh)
            strcpy(entry, "0");
        fresh = false;
        if (!strcmp(entry, "0"))
            entry[0] = '\0';
        if (strlen(entry) < DIGITS)
            strncat(entry, key, 1);
    } else if (c == '.') {
        if (fresh)
            strcpy(entry, "0");
        fresh = false;
        if (!strchr(entry, '.'))
            strcat(entry, ".");
    } else if (c == 'C') {
        strcpy(entry, "0");
        op = 0;
        fresh = true;
        error = false;
    } else if (c == '\b') {
        if (!fresh && strlen(entry) > 1 && strcmp(entry, "-0"))
            entry[strlen(entry) - 1] = '\0';
        else if (!fresh)
            strcpy(entry, "0");
    } else if (!strcmp(key, "+/-")) {
        show(-value());
    } else if (c == '%') {
        show(value() / 100);
        fresh = true;
    } else { // an operator, or =
        if (op && (!fresh || c == '=')) { // = again does it again
            double b = value(), result = apply(before, op, b);

            show(result);
            if (!error)
                remember(before, op, b, result);
        }
        before = value();
        op = c == '=' ? 0 : c;
        fresh = true;
    }
    printf("calculator: %s%s\n", error ? "Error" : entry, op ? (char[]){' ', op, '\0'} : "");
}

// Typing works too: digits and operators, Enter for =, Escape to clear, and
// Backspace. They're taken from the frame's keys, so Enter can't press a button.
static void typed(struct gui_ui *ui) {
    int kept = 0;

    for (int i = 0; i < ui->key_count; i++) {
        struct key_event *k = &ui->keys[i];
        char t = (char)k->text, key[2] = {t, '\0'};

        if (k->code == KEY_ENTER || t == '=')
            press("=");
        else if (k->code == KEY_ESCAPE)
            press("C");
        else if (k->code == KEY_BACKSPACE)
            press("\b");
        else if (t && strchr("0123456789.+-*/%", t))
            press(key);
        else
            ui->keys[kept++] = *k;
    }
    ui->key_count = kept;
}

static void copy_answer(void) {
    if (!error && gui_clipboard_set(entry, strlen(entry)))
        printf("calculator: copied %s\n", entry);
}

// A number from the clipboard, in place of what's being typed.
static void paste_number(void) {
    char *text = gui_clipboard_get(NULL), *end;
    double v = text ? strtod(text, &end) : 0;

    if (text && end != text) {
        show(v);
        fresh = true;
        printf("calculator: pasted %s\n", entry);
    }
    free(text);
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    if (gui_shortcut(ui, 'c'))
        copy_answer();
    if (gui_shortcut(ui, 'v'))
        paste_number();
    typed(ui);
    gui_column_begin(ui, PAD);
    gui_display(ui, error ? "Error" : entry, DISPLAY_HEIGHT);

    struct gui_rect display = gui_last_rect(ui);
    int width = (gui_room(ui) - 3 * GAP) / 4;

    for (int i = 0; i < KEY_COUNT; i++) {
        if (i % 4)
            gui_same_line(ui);
        if (gui_button_sized(ui, keys[i], i == 16 ? 2 * width + GAP : width, BUTTON_HEIGHT))
            press(keys[i]);
    }
    gui_column_end(ui);

    // The sums done, which put their answer back when one's chosen.
    gui_column_begin(ui, gui_room(ui));
    gui_label(ui, "History");
    gui_same_line(ui);
    if (gui_button(ui, "Clear")) {
        history_count = 0;
        history_selected = -1;
    }
    if (gui_list(ui, "##history", history_items, history_count, &history_selected, 0) && history_selected >= 0) {
        show(answers[history_selected]);
        fresh = true;
        printf("calculator: %s\n", entry);
    }
    gui_column_end(ui);
    if (gui_context_menu(ui, "##display", display)) {
        if (gui_menu_icon_item(ui, "Copy", gui_icon("copy")))
            copy_answer();
        if (gui_menu_icon_item(ui, "Paste", gui_icon("clipboard")))
            paste_number();
        gui_menu_end(ui);
    }
    gui_ui_end(ui);
    fflush(stdout);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: calculator\n"
                            "Adds, takes away, multiplies and divides, clicked or typed.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "calculator: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Calculator", 20 + PAD + 8 + HISTORY_WIDTH, 2 * 10 + DISPLAY_HEIGHT + 5 * (BUTTON_HEIGHT + GAP));
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "calculator: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    frame(&ui);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
