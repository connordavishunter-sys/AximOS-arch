#include <aximos.h>
#include "syntax.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)
#define MAX_DOCS 8
#define MAX_RECENT 6
#define FIND "##find"
#define REPLACE "##replace"
#define LINE "##line"
#define KEY_F3 (KEY_F1 + 2)

static const char *const help = "Ctrl+N        New tab\n"
                                "Ctrl+O        Open\n"
                                "Ctrl+S        Save\n"
                                "Ctrl+Shift+S  Save as\n"
                                "Ctrl+W        Close the tab\n"
                                "Ctrl+Tab      Next tab\n"
                                "Ctrl+Q        Quit\n"
                                "Ctrl+Z, Y     Undo and redo\n"
                                "Ctrl+F, H, G  Find, replace, go to a line\n"
                                "F3            Find the next\n"
                                "Tab           Indent, Shift+Tab unindent\n"
                                "F1            This help";

// Each tab's file: its text, a label for its widget, and what was selected in it.
struct doc {
    char *text, path[GUI_PATH_MAX], label[16];
    bool modified;
    size_t from, to;
};

static struct gui_window *window;
static struct gui_file_picker picker;
static struct doc docs[MAX_DOCS];
static int doc_count, active;
static char status[GUI_PATH_MAX + 64];
static bool line_numbers = true, wrap, quitting;

// Closing a tab with changes asks whether to save them first. Quitting closes
// each tab in turn, and saving one that has no name yet picks one first.
static int asking = -1; // the tab asked about
static bool closing_all, close_after_save;

static char recent[MAX_RECENT][GUI_PATH_MAX];
static int recent_count;

// The bar under the buttons, for finding, replacing or going to a line.
static enum { NO_BAR, FINDING, REPLACING, GOING } bar;
static bool bar_focus;
static char wanted[256], replacement[256], line_number[16];

#define DOC (&docs[active])

static const char *name_of(const struct doc *d) {
    const char *slash = strrchr(d->path, '/');

    return d->path[0] ? (slash ? slash + 1 : d->path) : "Untitled";
}

// "*name - Editor", with the star while there are changes to save.
static void show_title(void) {
    char title[GUI_TITLE_MAX];

    snprintf(title, sizeof(title), "%s%s - Editor", DOC->modified ? "*" : "", name_of(DOC));
    gui_set_title(window, title);
    printf("editor: title %s\n", title);
}

static void set_modified(struct doc *d, bool now) {
    if (d->modified == now)
        return;
    d->modified = now;
    if (d == DOC)
        show_title();
}

// Line numbers, wrapping and the files opened last are kept in ~/.editor.
static void settings_path(char path[GUI_PATH_MAX]) {
    const char *home = getenv("HOME");

    snprintf(path, GUI_PATH_MAX, "%s/.editor", home ? home : "");
}

static void save_settings(void) {
    char path[GUI_PATH_MAX];
    FILE *f;

    settings_path(path);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "numbers %d\nwrap %d\n", line_numbers, wrap);
    for (int i = 0; i < recent_count; i++)
        fprintf(f, "recent %s\n", recent[i]);
    fclose(f);
}

static void load_settings(void) {
    char path[GUI_PATH_MAX], line[GUI_PATH_MAX + 16];
    FILE *f;

    settings_path(path);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, "numbers ", 8))
            line_numbers = atoi(line + 8);
        else if (!strncmp(line, "wrap ", 5))
            wrap = atoi(line + 5);
        else if (!strncmp(line, "recent ", 7) && recent_count < MAX_RECENT)
            snprintf(recent[recent_count++], GUI_PATH_MAX, "%s", line + 7);
    }
    fclose(f);
}

// The newest first, once each.
static void add_recent(const char *path) {
    int i = 0;

    while (i < recent_count && strcmp(recent[i], path))
        i++;
    if (i == recent_count && recent_count < MAX_RECENT)
        recent_count++;
    for (i = i < MAX_RECENT ? i : MAX_RECENT - 1; i > 0; i--)
        memcpy(recent[i], recent[i - 1], GUI_PATH_MAX);
    snprintf(recent[0], GUI_PATH_MAX, "%s", path);
    save_settings();
}

static void switch_to(int index) {
    active = index;
    show_title();
    printf("editor: tab %d of %d\n", active + 1, doc_count);
}

// A new tab, returning false if there's no room for one.
static bool new_doc(void) {
    static int made;
    struct doc *d = &docs[doc_count];

    if (doc_count == MAX_DOCS || !(d->text = malloc(TEXT_MAX))) {
        snprintf(status, sizeof(status), "Can't open more than %d tabs", MAX_DOCS);
        return false;
    }
    d->text[0] = d->path[0] = '\0';
    d->modified = false;
    d->from = d->to = 0;
    snprintf(d->label, sizeof(d->label), "##text%d", made++);
    doc_count++;
    switch_to(doc_count - 1);
    return true;
}

static void close_doc(int index) {
    free(docs[index].text);
    memmove(&docs[index], &docs[index + 1], (doc_count - index - 1) * sizeof(*docs));
    doc_count--;
    if (!doc_count)
        quitting = true;
    else
        switch_to(active > index || active == doc_count ? active - 1 : active);
}

// Opens a file in a new tab, or in this one if it's empty and unchanged, or goes
// to the tab it's open in.
static void open_file(const char *chosen) {
    for (int i = 0; i < doc_count; i++) {
        if (!strcmp(docs[i].path, chosen)) {
            switch_to(i);
            return;
        }
    }

    // A file that isn't there yet is a new one, saved by that name.
    int fd = open(chosen, O_RDONLY);
    bool missing = fd < 0 && errno == ENOENT;

    if (fd < 0 && !missing) {
        snprintf(status, sizeof(status), "Can't open %s: %s", chosen, strerror(errno));
        return;
    }
    if ((DOC->text[0] || DOC->path[0] || DOC->modified) && !new_doc()) {
        if (fd >= 0)
            close(fd);
        return;
    }
    if (missing) {
        DOC->text[0] = '\0';
        snprintf(DOC->path, sizeof(DOC->path), "%s", chosen);
        snprintf(status, sizeof(status), "New file %s", chosen);
        printf("editor: new file %s\n", chosen);
        show_title();
        return;
    }

    ssize_t n = read(fd, DOC->text, TEXT_MAX);

    close(fd);
    if (n < 0 || n == TEXT_MAX) {
        snprintf(status, sizeof(status), n < 0 ? "Can't open %s: %s" : "%s is too big, over 256 kB", chosen,
                 strerror(errno));
        DOC->text[0] = '\0';
        return;
    }
    DOC->text[n] = '\0';
    DOC->from = DOC->to = 0;
    snprintf(DOC->path, sizeof(DOC->path), "%s", chosen);
    snprintf(status, sizeof(status), "Opened %s, %zd bytes", chosen, n);
    printf("editor: opened %s %zd bytes\n", chosen, n);
    add_recent(chosen);
    show_title();
}

static bool save_file(struct doc *d, const char *to) {
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t length = strlen(d->text);
    ssize_t n = fd < 0 ? -1 : write(fd, d->text, length);

    if (fd >= 0 && close(fd) < 0)
        n = -1;
    if (n != (ssize_t)length) {
        snprintf(status, sizeof(status), "Can't save %s: %s", to, strerror(errno));
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", to);
    snprintf(status, sizeof(status), "Saved %s, %zu bytes", to, length);
    printf("editor: saved %s %zu bytes\n", to, length);
    add_recent(to);
    d->modified = true;
    set_modified(d, false);
    show_title();
    return true;
}

// The picker starts in the folder of the file being edited, or the home folder.
static void pick(bool saving) {
    char folder[GUI_PATH_MAX];
    char *slash;

    snprintf(folder, sizeof(folder), "%s", DOC->path[0] ? DOC->path : getenv("HOME") ? getenv("HOME") : "/");
    slash = DOC->path[0] ? strrchr(folder, '/') : NULL;
    if (slash == folder)
        folder[1] = '\0';
    else if (slash)
        *slash = '\0';
    gui_picker_open(&picker, folder, saving && DOC->path[0] ? name_of(DOC) : "", saving);
    gui_post(&(struct gui_message){.type = GUI_REDRAW}); // to show it at once
}

static void save(void) {
    if (DOC->path[0])
        save_file(DOC, DOC->path);
    else
        pick(true);
}

// Closes a tab, asking first if it has changes.
static void close_tab(int index) {
    if (!docs[index].modified) {
        close_doc(index);
        return;
    }
    switch_to(index);
    asking = index;
    printf("editor: asking about %s\n", name_of(DOC));
}

static void quit(void) {
    closing_all = true;
}

// Quitting goes through the tabs, closing those without changes and asking about
// the first with them.
static void close_next(void) {
    while (closing_all && asking < 0 && !picker.open && doc_count) {
        int i = 0;

        while (i < doc_count && !docs[i].modified)
            i++;
        if (i == doc_count) {
            quitting = true;
            return;
        }
        close_tab(i);
    }
}

static void answer(int button) {
    int index = asking;

    asking = -1;
    if (button == 1 && docs[index].path[0]) {
        if (save_file(&docs[index], docs[index].path))
            close_doc(index);
        else
            closing_all = false;
    } else if (button == 1) {
        close_after_save = true;
        pick(true);
    } else if (button == 2) {
        close_doc(index);
    } else {
        closing_all = false;
        printf("editor: close cancelled\n");
    }
}

// The text gets the keyboard, but not this frame's keys, which were for the bar.
static void select_text(struct gui_ui *ui, size_t from, size_t to) {
    gui_select_text(ui, DOC->label, from, to);
    ui->key_count = 0;
    DOC->from = from;
    DOC->to = to;
    printf("editor: selected %zu %zu\n", from, to);
}

// Finds what's wanted after what's selected, going round to the start, and selects it.
static bool find_next(struct gui_ui *ui) {
    const char *text = DOC->text;
    size_t length = strlen(wanted), after = DOC->from > DOC->to ? DOC->from : DOC->to;
    const char *found = length ? strstr(text + after, wanted) : NULL;

    found = found || !length ? found : strstr(text, wanted);
    if (!found) {
        snprintf(status, sizeof(status), length ? "Can't find %s" : "Nothing to find", wanted);
        return false;
    }
    select_text(ui, found - text, found - text + length);
    snprintf(status, sizeof(status), "Found %s", wanted);
    return true;
}

// Puts the replacement in place of count characters at, if it fits.
static bool replace_at(size_t at, size_t count) {
    char *text = DOC->text;
    size_t length = strlen(text), with = strlen(replacement);

    if (length - count + with >= TEXT_MAX)
        return false;
    memmove(text + at + with, text + at + count, length - at - count + 1);
    memcpy(text + at, replacement, with);
    set_modified(DOC, true);
    return true;
}

// Replaces what's selected, if it's what's wanted, then finds the next.
static void replace_one(struct gui_ui *ui) {
    size_t low = DOC->from < DOC->to ? DOC->from : DOC->to, high = DOC->from < DOC->to ? DOC->to : DOC->from;
    size_t length = strlen(wanted);

    if (length && high - low == length && !strncmp(DOC->text + low, wanted, length) && replace_at(low, length))
        DOC->from = DOC->to = low + strlen(replacement);
    find_next(ui);
}

static void replace_all(struct gui_ui *ui) {
    size_t length = strlen(wanted), at = 0;
    int count = 0;

    for (char *found; length && (found = strstr(DOC->text + at, wanted)); count++) {
        at = found - DOC->text;
        if (!replace_at(at, length))
            break;
        at += strlen(replacement);
    }
    snprintf(status, sizeof(status), "Replaced %d", count);
    printf("editor: replaced %d\n", count);
    select_text(ui, at, at);
}

static void go_to_line(struct gui_ui *ui) {
    long line = strtol(line_number, NULL, 10), at_line = 1;
    size_t at = 0;

    for (; at_line < line && DOC->text[at]; at++)
        at_line += DOC->text[at] == '\n';
    snprintf(status, sizeof(status), "Line %ld", at_line);
    select_text(ui, at, at);
    bar = NO_BAR;
}

// The menu's Undo, Redo, Cut, Copy, Paste and Select all are the keys, sent to the text.
static void send_key(struct gui_ui *ui, char letter) {
    static const uint16_t codes[26] = {['a' - 'a'] = 0x1E, ['c' - 'a'] = 0x2E, ['v' - 'a'] = 0x2F,
                                       ['x' - 'a'] = 0x2D, ['y' - 'a'] = 0x15, ['z' - 'a'] = 0x2C};

    gui_select_text(ui, DOC->label, DOC->from, DOC->to);
    gui_post(&(struct gui_message){
        .type = GUI_KEY, .key = {.code = codes[letter - 'a'], .text = letter, .modifiers = KEY_MOD_CTRL, .pressed = true}});
}

static void open_bar(int which) {
    bar = which;
    bar_focus = true;
}

// A text box in the bar, which Enter does what it's for in.
static bool bar_box(struct gui_ui *ui, const char *prompt, const char *label, char *s, size_t size) {
    gui_label(ui, "%s", prompt);
    gui_same_line(ui);
    if (bar_focus)
        gui_set_focus(ui, label, s);
    bar_focus = false;
    gui_textbox(ui, label, s, size);
    gui_same_line(ui);
    return gui_focused(ui, label) && gui_key(ui, KEY_ENTER);
}

static void bar_strip(struct gui_ui *ui) {
    if (bar == GOING) {
        if (bar_box(ui, "Go to line", LINE, line_number, sizeof(line_number)) | gui_button(ui, "Go"))
            go_to_line(ui);
    } else {
        if (bar_box(ui, "Find", FIND, wanted, sizeof(wanted)) | gui_button(ui, "Next"))
            find_next(ui);
    }
    gui_same_line(ui);
    if (gui_button(ui, "Close") || gui_key(ui, KEY_ESCAPE))
        bar = NO_BAR;
    if (bar == REPLACING) {
        if (bar_box(ui, "With", REPLACE, replacement, sizeof(replacement)) | gui_button(ui, "Replace"))
            replace_one(ui);
        gui_same_line(ui);
        if (gui_button(ui, "All"))
            replace_all(ui);
    }
}

static void toggle(bool *option, const char *name) {
    *option = !*option;
    printf("editor: %s %s\n", name, *option ? "on" : "off");
    save_settings();
}

static void menus(struct gui_ui *ui) {
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "New tab"))
            new_doc();
        if (gui_menu_item(ui, "Open..."))
            pick(false);
        if (gui_menu_item(ui, "Save"))
            save();
        if (gui_menu_item(ui, "Save as..."))
            pick(true);
        if (gui_menu_item(ui, "Close tab"))
            close_tab(active);
        if (recent_count)
            gui_menu_separator(ui);
        for (int i = 0; i < recent_count; i++) {
            char label[GUI_PATH_MAX + 8];

            const char *slash = strrchr(recent[i], '/');

            snprintf(label, sizeof(label), "%s##recent%d", slash ? slash + 1 : recent[i], i);
            if (gui_menu_item(ui, label)) {
                char chosen[GUI_PATH_MAX];

                snprintf(chosen, sizeof(chosen), "%s", recent[i]);
                open_file(chosen);
            }
        }
        gui_menu_separator(ui);
        if (gui_menu_item(ui, "Quit"))
            quit();
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "Edit")) {
        static const char *const items[] = {"Undo", "Redo", "Cut", "Copy", "Paste", "Select all"};
        static const char letters[] = "zyxcva";

        for (int i = 0; i < 6; i++) {
            if (i == 2)
                gui_menu_separator(ui);
            if (gui_menu_item(ui, items[i]))
                send_key(ui, letters[i]);
        }
        gui_menu_separator(ui);
        if (gui_menu_item(ui, "Find..."))
            open_bar(FINDING);
        if (gui_menu_item(ui, "Find next"))
            find_next(ui);
        if (gui_menu_item(ui, "Replace..."))
            open_bar(REPLACING);
        if (gui_menu_item(ui, "Go to line..."))
            open_bar(GOING);
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "View")) {
        if (gui_menu_item(ui, line_numbers ? "Hide line numbers" : "Show line numbers"))
            toggle(&line_numbers, "line numbers");
        if (gui_menu_item(ui, wrap ? "Don't wrap lines" : "Wrap lines"))
            toggle(&wrap, "wrapping");
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "Help")) {
        if (gui_menu_item(ui, "Keyboard shortcuts"))
            gui_help_open(ui);
        gui_menu_end(ui);
    }
}

static void editing(struct gui_ui *ui) {
    static const char *const buttons[] = {"Save", "Don't save", "Cancel"};
    char question[GUI_PATH_MAX + 64];

    if (asking >= 0) {
        int button;

        snprintf(question, sizeof(question), "Save the changes to %s?\nThey're lost if you don't.", name_of(&docs[asking]));
        if ((button = gui_dialog(ui, question, buttons, 3)))
            answer(button);
    } else {
        gui_help(ui, help);
    }
    menus(ui);

    // Tabs show when there's more than one.
    if (doc_count > 1) {
        const char *names[MAX_DOCS];
        char marked[MAX_DOCS][GUI_TITLE_MAX];
        int closing = -1, result;

        for (int i = 0; i < doc_count; i++) {
            snprintf(marked[i], sizeof(marked[i]), "%s%s", docs[i].modified ? "*" : "", name_of(&docs[i]));
            names[i] = marked[i];
        }
        result = gui_tabs(ui, names, doc_count, &active, &closing);
        if (result == GUI_TABS_CHANGED)
            switch_to(active);
        else if (result == GUI_TABS_NEW)
            new_doc();
        else if (result == GUI_TABS_CLOSE)
            close_tab(closing);
    } else if (gui_shortcut(ui, 'w')) {
        close_tab(active);
    }
    if (quitting)
        return;
    if (gui_shortcut(ui, 'f'))
        open_bar(FINDING);
    if (gui_shortcut(ui, 'h'))
        open_bar(REPLACING);
    if (gui_shortcut(ui, 'g'))
        open_bar(GOING);
    if (gui_key(ui, KEY_F3))
        find_next(ui);
    if (gui_shortcut(ui, 'n'))
        new_doc();
    if (gui_shortcut(ui, 'o'))
        pick(false);
    if (gui_shortcut(ui, 's'))
        save();
    if (gui_shortcut(ui, 'S'))
        pick(true);

    if (gui_button(ui, "Open..."))
        pick(false);
    gui_same_line(ui);
    if (gui_button(ui, "Save"))
        save();
    gui_same_line(ui);
    gui_label(ui, "%s", status);
    if (bar)
        bar_strip(ui);

    gui_highlighter *highlight = syntax_for(DOC->path, DOC->text);
    static gui_highlighter *was = (gui_highlighter *)1;

    if (highlight != was)
        printf("editor: colouring %s\n", syntax_name(highlight));
    was = highlight;

    int flags = GUI_CODE_INDENT | (line_numbers ? GUI_CODE_LINE_NUMBERS : 0) | (wrap ? GUI_CODE_WRAP : 0);

    if (gui_code_area(ui, DOC->label, DOC->text, TEXT_MAX, 0, highlight, flags))
        set_modified(DOC, true);
    if (gui_focused(ui, DOC->label)) {
        DOC->from = ui->text_anchor;
        DOC->to = ui->text_cursor;
    }

    // Right-clicking the text has what the Edit menu has.
    if (gui_context_menu(ui, "##edit", gui_last_rect(ui))) {
        static const char *const items[] = {"Cut", "Copy", "Paste", "Select all"};

        for (int i = 0; i < 4; i++) {
            if (gui_menu_item(ui, items[i]))
                send_key(ui, "xcva"[i]);
        }
        gui_menu_end(ui);
    }
}

static void frame(struct gui_ui *ui) {
    static char picking_in[GUI_PATH_MAX];

    close_next();
    if (quitting)
        return;
    gui_ui_begin(ui);
    if (!picker.open) {
        editing(ui);
    } else {
        bool saving = picker.saving;
        int result = gui_file_picker(ui, &picker);

        if (result == GUI_PICKED && saving && save_file(DOC, picker.path) && close_after_save)
            close_doc(active);
        else if (result == GUI_PICKED && !saving)
            open_file(picker.path);
        else if (result == GUI_PICK_CANCELLED)
            printf("editor: picking cancelled\n");
        if (result && close_after_save && result == GUI_PICK_CANCELLED)
            closing_all = false;
        if (result)
            close_after_save = false;
        if (picker.open && strcmp(picking_in, picker.folder))
            printf("editor: picking in %s\n", picker.folder);
        snprintf(picking_in, sizeof(picking_in), "%s", picker.open ? picker.folder : "");

        // The frame after shows the text again.
        if (result)
            gui_post(&(struct gui_message){.type = GUI_REDRAW});
    }
    if (!quitting)
        gui_ui_end(ui);
    fflush(stdout);
}

// A window to write text in, saved to a file, with a tab for each file open.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: editor [file]...\n"
                            "Opens a window to edit the files in, or a new one.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "editor: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;

    window = gui_create_window("Editor", 640, 480);
    if (!window || !new_doc()) {
        dprintf(STDERR_FILENO, "editor: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    load_settings();
    for (int i = 1; i < argc; i++)
        open_file(argv[i]);
    frame(&ui);
    while (!quitting && gui_next_event(&e)) {
        char dropped[GUI_PATH_MAX];
        bool opening = gui_dropped_file(&e, dropped);

        if (e.type == GUI_CLOSE)
            quit();
        if (opening)
            open_file(dropped);
        if (gui_ui_event(&ui, &e) || e.type == GUI_CLOSE || opening)
            frame(&ui);
    }
    printf("editor: closed\n");
    gui_destroy_window(window);
    return 0;
}
