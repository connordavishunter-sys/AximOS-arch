#include <dirent.h>
#include <errno.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ENTRIES 1024
#define NAME_BOX "##name"

static void forget(struct gui_file_picker *p) {
    for (int i = 0; i < p->count; i++)
        free(p->items[i]);
    free(p->items);
    free(p->icons);
    p->items = NULL;
    p->icons = NULL;
    p->count = p->selected = 0;
}

// A name in the folder, or a path of its own if it starts with /.
static void full_path(const struct gui_file_picker *p, const char *name, char out[GUI_PATH_MAX]) {
    if (name[0] == '/')
        snprintf(out, GUI_PATH_MAX, "%s", name);
    else
        snprintf(out, GUI_PATH_MAX, "%s%s%s", p->folder, strcmp(p->folder, "/") ? "/" : "", name);
}

static bool is_folder(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Folders, which end in /, first, then by name.
static int compare_items(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    bool x_folder = x[strlen(x) - 1] == '/', y_folder = y[strlen(y) - 1] == '/';

    return x_folder != y_folder ? y_folder - x_folder : strcmp(x, y);
}

// Cuts a path to the folder it's in.
static void cut_to_parent(char *path) {
    char *slash = strrchr(path, '/');

    if (slash == path)
        path[1] = '\0';
    else if (slash)
        *slash = '\0';
}

// Goes to a folder, with .. going up, returning false if it can't be read.
static bool read_folder(struct gui_file_picker *p, const char *path) {
    char folder[GUI_PATH_MAX];
    DIR *dir;
    struct dirent *d;

    snprintf(folder, sizeof(folder), "%s", path);
    for (size_t n = strlen(folder); n > 1 && folder[n - 1] == '/'; n--)
        folder[n - 1] = '\0';

    size_t n = strlen(folder);

    if (n >= 3 && !strcmp(folder + n - 3, "/..")) {
        folder[n - 3 ? n - 3 : 1] = '\0';
        cut_to_parent(folder);
    }
    dir = opendir(folder);
    if (!dir) {
        snprintf(p->status, sizeof(p->status), "Can't open %s: %s", folder, strerror(errno));
        return false;
    }
    forget(p);
    snprintf(p->folder, sizeof(p->folder), "%s", folder);
    p->items = malloc(MAX_ENTRIES * sizeof(*p->items));
    while (p->items && p->count < MAX_ENTRIES && (d = readdir(dir))) {
        char path_of[GUI_PATH_MAX];
        size_t length = strlen(d->d_name);

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        full_path(p, d->d_name, path_of);

        bool folder_too = is_folder(path_of);
        char *item = malloc(length + 2);

        if (!item)
            break;
        memcpy(item, d->d_name, length);
        strcpy(item + length, folder_too ? "/" : "");
        p->items[p->count++] = item;
    }
    closedir(dir);
    qsort(p->items, p->count, sizeof(*p->items), compare_items);
    p->icons = malloc((p->count ? p->count : 1) * sizeof(*p->icons));
    for (int i = 0; p->icons && i < p->count; i++)
        p->icons[i] = gui_file_icon(p->items[i], p->items[i][strlen(p->items[i]) - 1] == '/');
    snprintf(p->status, sizeof(p->status), "%d item%s", p->count, p->count == 1 ? "" : "s");
    p->replacing[0] = '\0';
    return true;
}

void gui_picker_open(struct gui_file_picker *p, const char *folder, const char *name, bool saving) {
    p->open = p->focus_name = true;
    p->saving = saving;
    p->path[0] = '\0';
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
    if (!read_folder(p, folder && folder[0] ? folder : "/"))
        read_folder(p, "/");
}

static int finish(struct gui_file_picker *p, int result) {
    forget(p);
    p->open = false;
    return result;
}

// What's in the name box: a folder goes there, and a file is chosen, if it's there
// to open, or once it's said to replace one that's there when saving.
static int choose(struct gui_file_picker *p) {
    char path[GUI_PATH_MAX];

    if (!p->name[0]) {
        snprintf(p->status, sizeof(p->status), "Choose a file, or type its name");
        return 0;
    }
    full_path(p, p->name, path);
    if (is_folder(path)) {
        if (read_folder(p, path))
            p->name[0] = '\0';
        return 0;
    }

    struct stat st;
    bool there = stat(path, &st) == 0;

    if (!p->saving && !there) {
        snprintf(p->status, sizeof(p->status), "There's no %s", p->name);
        return 0;
    }
    if (p->saving && there && strcmp(p->replacing, path)) {
        snprintf(p->replacing, sizeof(p->replacing), "%s", path);
        snprintf(p->status, sizeof(p->status), "%s is already there: Save again to replace it", p->name);
        return 0;
    }
    snprintf(p->path, sizeof(p->path), "%s", path);
    return finish(p, GUI_PICKED);
}

static void go_up(struct gui_file_picker *p) {
    char above[GUI_PATH_MAX];

    snprintf(above, sizeof(above), "%s", p->folder);
    cut_to_parent(above);
    read_folder(p, above);
}

int gui_file_picker(struct gui_ui *ui, struct gui_file_picker *p) {
    if (!p->open)
        return 0;
    if (p->focus_name)
        gui_set_focus(ui, NAME_BOX, p->name);
    p->focus_name = false;
    if (gui_key(ui, KEY_ESCAPE))
        return finish(p, GUI_PICK_CANCELLED);

    gui_heading(ui, p->saving ? "Save as" : "Open a file");
    if (gui_button(ui, "Up"))
        go_up(p);
    gui_same_line(ui);
    gui_label(ui, "%s", p->folder);

    bool chosen = gui_button(ui, p->saving ? "Save" : "Open");

    gui_same_line(ui);
    if (gui_button(ui, "Cancel"))
        return finish(p, GUI_PICK_CANCELLED);
    gui_same_line(ui);
    if (gui_textbox(ui, NAME_BOX, p->name, sizeof(p->name)))
        p->replacing[0] = '\0';
    gui_label(ui, "%s", p->status);

    // Choosing a file puts its name in the box, and double-clicking it or a
    // folder opens that.
    int was = p->selected;
    int picked = gui_list_icons(ui, "##files", (const char *const *)p->items, p->icons, p->count, &p->selected, 0);
    char *item = p->count ? p->items[p->selected] : NULL;
    bool item_folder = item && item[strlen(item) - 1] == '/';

    if (picked && item && !item_folder && (picked == GUI_LIST_ACTIVATED || was != p->selected)) {
        snprintf(p->name, sizeof(p->name), "%s", item);
        p->replacing[0] = '\0';
    }
    if (picked == GUI_LIST_ACTIVATED && item_folder) {
        char path[GUI_PATH_MAX];

        full_path(p, item, path);
        read_folder(p, path);
        return 0;
    }
    if (picked == GUI_LIST_ACTIVATED || chosen || (picked != GUI_LIST_ACTIVATED && gui_key(ui, KEY_ENTER)))
        return choose(p);
    return 0;
}
