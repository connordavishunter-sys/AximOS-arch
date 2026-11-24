#include <aximos.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const themes[] = {"Dark", "Light", "Solarized", "Nord"};
static const char *const months[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};
static const char *const fruits[] = {
    "Apple", "Apricot", "Banana", "Blackberry", "Blueberry", "Cherry", "Coconut", "Date",  "Fig",   "Grape",
    "Guava", "Kiwi",    "Lemon",  "Lime",       "Mango",     "Melon",  "Orange",  "Peach", "Pear",  "Plum",
};
#define COUNT(a) ((int)(sizeof(a) / sizeof(a[0])))

static int count;
static bool show_progress = true, quitting;
static int volume = 50, theme, month, fruit;
static char name[32], notes[256];

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[16];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)) != 0)
        printf("widgets: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

// Notes go on one line, with | for each new line.
static void print_notes(void) {
    printf("widgets: notes ");
    for (const char *p = notes; *p; p++)
        putchar(*p == '\n' ? '|' : *p);
    putchar('\n');
}

static void toggle_progress(void) {
    show_progress = !show_progress;
    printf("widgets: progress %s\n", show_progress ? "shown" : "hidden");
}

static void menus(struct gui_ui *ui) {
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "Reset count"))
            printf("widgets: count %d\n", count = 0);
        if (gui_menu_item(ui, "Quit"))
            quitting = true;
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "View")) {
        if (gui_menu_item(ui, show_progress ? "Hide progress" : "Show progress"))
            toggle_progress();
        gui_menu_end(ui);
    }
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    menus(ui);
    gui_scroll_begin(ui, "##page", 0);
    gui_heading(ui, "Widgets");
    gui_label(ui, "Drawn by libgui, a frame at a time.");
    gui_separator(ui);

    if (gui_button(ui, "Count"))
        printf("widgets: count %d\n", ++count);
    report(ui, 0, "count");
    gui_same_line(ui);
    if (gui_button(ui, "Reset"))
        printf("widgets: count %d\n", count = 0);
    report(ui, 1, "reset");
    gui_same_line(ui);
    gui_label(ui, "pressed %d times", count);

    bool shown = show_progress;

    if (gui_checkbox(ui, "Show progress", &shown))
        toggle_progress();
    report(ui, 2, "checkbox");
    if (gui_slider(ui, "Volume", &volume, 0, 100))
        printf("widgets: volume %d\n", volume);
    report(ui, 3, "slider");
    if (show_progress)
        gui_progress(ui, volume / 100.0f);

    if (gui_textbox(ui, "Name", name, sizeof(name)))
        printf("widgets: name %s\n", name);
    report(ui, 4, "textbox");
    gui_label(ui, name[0] ? "Hello, %s!" : "Type your name above.", name);
    gui_separator(ui);

    if (gui_dropdown(ui, "Theme", themes, COUNT(themes), &theme))
        printf("widgets: theme %s\n", themes[theme]);
    report(ui, 5, "dropdown");
    if (gui_dropdown(ui, "Month", months, COUNT(months), &month))
        printf("widgets: month %s\n", months[month]);
    report(ui, 9, "month");
    if (gui_list(ui, "Fruit", fruits, COUNT(fruits), &fruit, 5))
        printf("widgets: fruit %s\n", fruits[fruit]);
    report(ui, 6, "list");
    if (gui_textarea(ui, "Notes", notes, sizeof(notes), 4))
        print_notes();
    report(ui, 7, "notes");
    gui_label(ui, "Tab moves, Enter or Space presses.");
    gui_scroll_end(ui);
    report(ui, 8, "page");
    gui_ui_end(ui);
    fflush(stdout);
}

// Every kind of widget libgui has, to try them out.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: widgets\n"
                            "Opens a window showing every kind of widget libgui has.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "widgets: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Widgets", 400, 420);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "widgets: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    frame(&ui);
    while (!quitting && gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
