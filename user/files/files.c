#include "drives.h"
#include <aximos.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 1024
#define MAX_TABS 8
#define MAX_APPS 32
#define LIST "##entries"
#define ADDRESS "##folder"
#define SEARCH "Search##search"
#define PLACES "##places"
#define PLACE_COUNT 7
#define RECENT "recent" // the place listing the files opened lately, which no path can be
#define SIDEBAR 120
#define CUT "/tmp/files-cut" // the paths on the clipboard, when they were cut, to move them
#define KEY_F2 (KEY_F1 + 1)
#define KEY_F5 (KEY_F1 + 4)
#define DRAG_DISTANCE 6 // the mouse goes, held on an item, before it's dragged

// In the trash, an entry's name is its random one there, and it shows the one it
// had. In Recent, its name is its whole path.
struct entry {
    char name[NAME_MAX + 1], shown[NAME_MAX + 1];
    bool folder;
    off_t size;
    time_t modified;
};

static const char *const help = "Enter         Open\n"
                                "Backspace     Up a folder\n"
                                "Ctrl+T, W     New tab, close the tab\n"
                                "Ctrl+Tab      Next tab\n"
                                "Ctrl+click    Choose another, or not\n"
                                "Shift+click   Choose those up to it\n"
                                "Ctrl+A        Choose everything\n"
                                "Ctrl+C, X, V  Copy, cut, paste\n"
                                "F2            Rename\n"
                                "Delete        Move to the trash\n"
                                "Shift+Delete  Delete for good\n"
                                "F5            Refresh\n"
                                "Ctrl+F        Search this folder and those in it\n"
                                "Ctrl+H        Show hidden files\n"
                                "Ctrl+Q        Quit\n"
                                "F1            This help";

// Entries are listed folders first, then by name, size or when they changed, the
// latter two biggest and newest first, leaving out those that start with a dot
// unless they're shown, and those without what's searched for in their names.
static enum { BY_NAME, BY_SIZE, BY_DATE } sort_by;
static bool show_hidden;
static char search[NAME_MAX + 1];

// The places down the side.
static const char *const place_names[PLACE_COUNT] = {"Home", "Desktop", "Documents", "Pictures", "Computer", "Trash", "Recent"};
static char place_paths[PLACE_COUNT][PATH_MAX];

// Each tab is a folder, and what was selected in it. The one shown is listed.
struct tab {
    char folder[PATH_MAX], selected[NAME_MAX + 1], name[NAME_MAX + 1];
};

static struct tab tabs[MAX_TABS];
static const char *tab_names[MAX_TABS];
static int tab_count, current;

static struct entry entries[MAX_ENTRIES];
static char labels[MAX_ENTRIES][NAME_MAX + 2]; // names, with / after folders
static const char *items[MAX_ENTRIES];
static const struct gui_image *icons[MAX_ENTRIES];
static bool marked[MAX_ENTRIES]; // those chosen, with the selected one
static int count, selected;
static char *folder = tabs[0].folder, typed[PATH_MAX], status[PATH_MAX + 64];
static bool quitting;

// In the grid, each entry has a picture: a picture's own, made small, or its icon
// made bigger, kept for each icon.
#define BIG_ICON 48
static bool grid;
static struct gui_image thumbnails[MAX_ENTRIES];
static const struct gui_image *pictures[MAX_ENTRIES];
static struct {
    const struct gui_image *icon;
    struct gui_image big;
} big_icons[8];

// A file dropped on the window, for the frame to put where it was dropped.
static char dropped[PATH_MAX];
static bool drop_copies;

// What the strip under the address bar, or a dialog, is asking, if anything.
static enum { NOTHING, RENAMING, OPENING_WITH, DELETING, EMPTYING, PROPERTIES } asking;

// What's being renamed where its name's shown, by the name it had, and what it's
// being called, and whether the box has just opened.
static char renaming[NAME_MAX + 1], new_name[NAME_MAX + 1];
static bool rename_started;
static bool ask_focus; // the app's dropdown gets the keyboard

// The apps, for Open with.
static char app_names[MAX_APPS][32];
static const char *apps[MAX_APPS];
static int app_count, app_chosen;

static bool in_trash(void) {
    return !strcmp(folder, GUI_TRASH);
}

static bool in_recent(void) {
    return !strcmp(folder, RECENT);
}

static void path_of(char *out, const char *name) {
    if (in_recent())
        snprintf(out, PATH_MAX, "%s", name);
    else
        snprintf(out, PATH_MAX, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", name);
}

static int compare_entries(const void *a, const void *b) {
    const struct entry *x = a, *y = b;

    if (x->folder != y->folder)
        return y->folder - x->folder;
    if (sort_by == BY_SIZE && x->size != y->size)
        return x->size < y->size ? 1 : -1;
    if (sort_by == BY_DATE && x->modified != y->modified)
        return x->modified < y->modified ? 1 : -1;
    return strcmp(x->shown, y->shown);
}

static bool has(const char *name, const char *part) {
    for (size_t n = strlen(part); *name; name++) {
        size_t i = 0;

        while (i < n && tolower((unsigned char)name[i]) == tolower((unsigned char)part[i]))
            i++;
        if (i == n)
            return true;
    }
    return !*part;
}

static const struct entry *chosen(void) {
    return count ? &entries[selected] : NULL;
}

// Only what's in the folder shown can be renamed there, not what Recent or a search
// found elsewhere.
static bool can_rename(void) {
    return !in_recent() && chosen() && !strchr(chosen()->name, '/');
}

// What's chosen, by index: those marked, or else the selected one.
static int chosen_items(int out[MAX_ENTRIES]) {
    int n = 0;

    for (int i = 0; i < count; i++) {
        if (marked[i])
            out[n++] = i;
    }
    if (!n && count)
        out[n++] = selected;
    return n;
}

static void describe(void) {
    const struct entry *e = chosen();
    int picked[MAX_ENTRIES], n = chosen_items(picked);

    if (!count)
        snprintf(status, sizeof(status), "%s", in_trash()    ? "The trash is empty"
                                             : in_recent() ? "Nothing's been opened lately"
                                             : search[0]   ? "Nothing here or in its folders has that in its name"
                                                           : folder);
    else if (n > 1)
        snprintf(status, sizeof(status), "%d items chosen", n);
    else if (e->folder)
        snprintf(status, sizeof(status), "%s: folder", e->shown);
    else
        snprintf(status, sizeof(status), "%s: %lld bytes", e->shown, (long long)e->size);
}

static void name_tab(struct tab *t) {
    const char *slash = strrchr(t->folder, '/');

    snprintf(t->name, sizeof(t->name), "%s", !strcmp(t->folder, GUI_TRASH) ? "Trash"
                                           : !strcmp(t->folder, RECENT)    ? "Recent"
                                           : slash && slash[1]             ? slash + 1
                                                                           : "/");
}

static const struct gui_image *big_icon(const struct gui_image *icon) {
    for (int i = 0; icon && i < (int)(sizeof(big_icons) / sizeof(big_icons[0])); i++) {
        if (big_icons[i].icon == icon)
            return &big_icons[i].big;
        if (!big_icons[i].icon && gui_image_scale(icon, BIG_ICON, BIG_ICON, &big_icons[i].big)) {
            big_icons[i].icon = icon;
            return &big_icons[i].big;
        }
    }
    return icon;
}

static void forget_pictures(void) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        gui_image_free(&thumbnails[i]);
        pictures[i] = NULL;
    }
}

// Pictures for the grid, made small to fit GUI_THUMBNAIL, never bigger than they are.
static void make_pictures(void) {
    forget_pictures();
    for (int i = 0; i < count; i++) {
        char path[PATH_MAX];
        struct gui_image full;

        path_of(path, entries[i].name);
        pictures[i] = big_icon(icons[i]);
        if (icons[i] != gui_icon("image") || !gui_image_load(path, &full))
            continue;

        int big = full.width > full.height ? full.width : full.height;

        if (big <= GUI_THUMBNAIL) {
            thumbnails[i] = full;
            pictures[i] = &thumbnails[i];
            continue;
        }
        if (gui_image_scale(&full, full.width * GUI_THUMBNAIL / big > 0 ? full.width * GUI_THUMBNAIL / big : 1,
                            full.height * GUI_THUMBNAIL / big > 0 ? full.height * GUI_THUMBNAIL / big : 1, &thumbnails[i]))
            pictures[i] = &thumbnails[i];
        gui_image_free(&full);
    }
}

static void set_grid(bool on) {
    grid = on;
    if (grid)
        make_pictures();
    else
        forget_pictures();
    printf("files: view %s\n", grid ? "grid" : "list");
    gui_post(&(struct gui_message){.type = GUI_REDRAW}); // with the button's new name
}

// What an entry shows: its own name, or in the trash, the one it had.
static void name_shown(struct entry *e) {
    char origin[GUI_PATH_MAX];

    if (in_trash() && gui_trash_origin(e->name, origin))
        snprintf(e->shown, sizeof(e->shown), "%s", strrchr(origin, '/') + 1);
    else if (in_recent())
        snprintf(e->shown, sizeof(e->shown), "%s", strrchr(e->name, '/') + 1);
    else
        memcpy(e->shown, e->name, sizeof(e->shown));
}

static bool listed(const char *select);

// Fills in an entry for a path, if it's still there and shown, returning whether it is.
static bool add_entry(const char *name) {
    struct entry *e = &entries[count];
    char full[PATH_MAX];
    struct stat st;

    snprintf(e->name, sizeof(e->name), "%s", name);
    name_shown(e);
    const char *base = strrchr(e->shown, '/') ? strrchr(e->shown, '/') + 1 : e->shown;

    if ((base[0] == '.' && !show_hidden) || !has(base, search))
        return false;
    path_of(full, e->name);
    if (stat(full, &st) < 0) {
        if (in_recent()) // gone since
            return false;
        memset(&st, 0, sizeof(st));
    }
    e->folder = S_ISDIR(st.st_mode);
    e->size = e->folder ? 0 : st.st_size;
    e->modified = st.st_mtime;
    count++;
    return true;
}

// Recent, the files opened lately that are still there, newest first.
static bool read_recent(const char *select) {
    char paths[GUI_RECENT_MAX][GUI_PATH_MAX];
    int n = gui_recent_list(paths, GUI_RECENT_MAX);

    if (!in_recent())
        search[0] = '\0';
    snprintf(folder, PATH_MAX, "%s", RECENT);
    name_tab(&tabs[current]);
    count = 0;
    for (int i = 0; i < n && count < MAX_ENTRIES; i++) {
        if (strlen(paths[i]) <= NAME_MAX)
            add_entry(paths[i]);
    }
    return listed(select);
}

// Searching looks in the folders inside too, as far as SEARCH_DEPTH down, listing
// what it finds by its path from here. /proc and /dev are left out.
#define SEARCH_DEPTH 8
static void search_below(const char *inside, int depth) {
    char path[PATH_MAX], name[PATH_MAX];
    DIR *dir;
    struct dirent *d;

    path_of(path, inside);
    if (depth > SEARCH_DEPTH || !strcmp(path, "/proc") || !strcmp(path, "/dev") || !(dir = opendir(path)))
        return;
    while (count < MAX_ENTRIES && (d = readdir(dir))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") || (d->d_name[0] == '.' && !show_hidden))
            continue;
        snprintf(name, sizeof(name), "%s%s%s", inside, *inside ? "/" : "", d->d_name);
        if (strlen(name) > NAME_MAX)
            continue;
        add_entry(name);
        if (d->d_type == DT_DIR)
            search_below(name, depth + 1);
    }
    closedir(dir);
}

// Lists a folder, going there in this tab, and choosing the entry called select if there is one.
static bool read_folder(const char *path, const char *select) {
    char went[PATH_MAX];
    DIR *dir;
    struct dirent *d;

    if (!strcmp(path, RECENT))
        return read_recent(select);
    if (!(dir = opendir(path))) {
        snprintf(status, sizeof(status), "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    snprintf(went, sizeof(went), "%s", path); // path may be folder itself
    for (size_t n = strlen(went); n > 1 && went[n - 1] == '/'; n--)
        went[n - 1] = '\0';
    if (strcmp(folder, went))
        search[0] = '\0'; // a search is in one folder
    snprintf(folder, PATH_MAX, "%s", went);
    name_tab(&tabs[current]);
    count = 0;
    if (search[0] && !in_trash())
        search_below("", 0);
    while (!search[0] && count < MAX_ENTRIES && (d = readdir(dir))) {
        if (strcmp(d->d_name, ".") && strcmp(d->d_name, ".."))
            add_entry(d->d_name);
    }
    closedir(dir);
    qsort(entries, count, sizeof(*entries), compare_entries);
    return listed(select);
}

// Shows the entries listed, in this tab, choosing the one called select if there is one.
static bool listed(const char *select) {
    printf("files: in %s:", folder);
    selected = 0;
    for (int i = 0; i < count; i++) {
        char path[PATH_MAX];

        path_of(path, entries[i].name);
        snprintf(labels[i], sizeof(labels[i]), "%s%s", entries[i].shown, entries[i].folder ? "/" : "");
        items[i] = labels[i];
        icons[i] = entries[i].folder ? gui_file_icon(entries[i].shown, true)
                 : gui_is_program(path) ? gui_icon("program")
                                        : gui_file_icon(entries[i].shown, false);
        printf(" %s", labels[i]);
        if (select && !strcmp(entries[i].name, select))
            selected = i;
    }
    printf("\n");
    memset(marked, 0, sizeof(marked));
    marked[selected] = count > 0;
    snprintf(typed, sizeof(typed), "%s", folder);
    asking = NOTHING;
    describe();
    if (grid)
        make_pictures();
    return true;
}

static void refresh(void) {
    char name[NAME_MAX + 1];

    snprintf(name, sizeof(name), "%s", count ? entries[selected].name : "");
    read_folder(folder, name);
}

static void go_up(void) {
    char parent[PATH_MAX], *slash;

    snprintf(parent, sizeof(parent), "%s", folder);
    slash = strrchr(parent, '/');
    if (!slash || !strcmp(folder, "/"))
        return;
    *slash = '\0';
    read_folder(parent[0] ? parent : "/", slash + 1);
}

// Keeps what's selected in this tab, for when it's shown again.
static void remember(void) {
    if (count)
        snprintf(tabs[current].selected, sizeof(tabs[current].selected), "%s", entries[selected].name);
}

static void show_tab(int index) {
    current = index;
    folder = tabs[current].folder;
    if (!read_folder(folder, tabs[current].selected))
        read_folder("/", NULL);
    printf("files: tab %d of %d\n", current + 1, tab_count);
}

static void new_tab(const char *path) {
    if (tab_count == MAX_TABS) {
        snprintf(status, sizeof(status), "There can be %d tabs at most", MAX_TABS);
        return;
    }
    snprintf(tabs[tab_count].folder, PATH_MAX, "%s", path);
    tabs[tab_count].selected[0] = '\0';
    tab_names[tab_count] = tabs[tab_count].name;
    remember();
    show_tab(tab_count++);
}

// The last tab closing closes the window.
static void close_tab(int index) {
    if (tab_count == 1) {
        quitting = true;
        return;
    }
    remember();
    memmove(&tabs[index], &tabs[index + 1], (tab_count - index - 1) * sizeof(*tabs));
    tab_count--;
    current = current > index || current == tab_count ? current - 1 : current;
    for (int i = 0; i < tab_count; i++)
        tab_names[i] = tabs[i].name;
    show_tab(current);
}

static void start(const char *program, const char *argument, const char *in) {
    if (fork() == 0) {
        if (in && chdir(in) < 0)
            _exit(127);
        execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
}

static void start_app(const char *name, const char *argument, const char *in) {
    char program[GUI_PATH_MAX];

    if (gui_app_path(name, program))
        start(program, argument, in);
}

static void open_in(const char *app) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    printf("files: opening %s in %s\n", path, app);
    gui_recent_add(path);
    start_app(app, path, NULL);
}

static void run_selected(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    printf("files: running %s\n", path);
    gui_recent_add(path);
    start(path, NULL, NULL);
}

// Folders open here, programs run, and other files open in the app the settings
// choose for their kind.
static void open_selected(void) {
    char path[PATH_MAX];

    if (!chosen())
        return;
    path_of(path, chosen()->name);
    if (chosen()->folder)
        read_folder(path, NULL);
    else if (gui_is_program(path))
        run_selected();
    else
        open_in(gui_app_for(path));
}

static void start_renaming(void) {
    if (!can_rename() || in_trash())
        return;
    snprintf(renaming, sizeof(renaming), "%s", chosen()->name);
    snprintf(new_name, sizeof(new_name), "%s", chosen()->name);
    rename_started = true;
    printf("files: renaming %s\n", renaming);
    gui_post(&(struct gui_message){.type = GUI_REDRAW});
}

static void ask_about(int what) {
    if (what == RENAMING)
        return start_renaming();
    asking = what;
    ask_focus = true;
    app_chosen = 0;
    gui_post(&(struct gui_message){.type = GUI_REDRAW}); // with the strip or dialog asking
}

static void copy_path(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    gui_clipboard_set(path, strlen(path));
    snprintf(status, sizeof(status), "Copied %s", path);
}

// Puts what's chosen on the clipboard, by their paths, a line each, which pasting
// copies, or moves when they were cut.
static void copy_selected(bool cut) {
    static char text[MAX_ENTRIES * 64];
    int picked[MAX_ENTRIES], n = chosen_items(picked);
    size_t used = 0;
    FILE *f;

    for (int i = 0; i < n && used < sizeof(text) - PATH_MAX; i++) {
        path_of(text + used, entries[picked[i]].name);
        used += strlen(text + used);
        text[used++] = '\n';
    }
    text[used ? used - 1 : 0] = '\0';
    if (!gui_clipboard_set(text, strlen(text)))
        return;
    unlink(CUT);
    if (cut && (f = fopen(CUT, "w"))) {
        fputs(text, f);
        fclose(f);
    }
    snprintf(status, sizeof(status), "%s %d item%s", cut ? "Cut" : "Copied", n, n == 1 ? "" : "s");
    printf("files: %s %s\n", cut ? "cut" : "copied", text);
}

// A name in a folder that isn't taken: the same, or with (copy), (copy 2) and so on.
static void free_name(const char *into, const char *name, char out[NAME_MAX + 1]) {
    char path[PATH_MAX];
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

    snprintf(out, NAME_MAX + 1, "%s", name);
    for (int i = 1; snprintf(path, sizeof(path), "%s%s%s", into, strcmp(into, "/") ? "/" : "", out), access(path, F_OK) == 0;
         i++) {
        if (i == 1)
            snprintf(out, NAME_MAX + 1, "%.*s (copy)%s", stem, name, name + stem);
        else
            snprintf(out, NAME_MAX + 1, "%.*s (copy %d)%s", stem, name, i, name + stem);
    }
}

// Copies a file or folder into a folder, or moves it there, with a name that isn't
// taken, saying what it did as done. Returns where it went, or NULL.
static const char *put(const char *from, const char *into, bool moving, const char *done) {
    static char to[PATH_MAX];
    char name[NAME_MAX + 1], here[PATH_MAX];
    const char *base = strrchr(from, '/') ? strrchr(from, '/') + 1 : from;
    size_t length = strlen(from);
    bool ok;

    snprintf(here, sizeof(here), "%s%s%s", into, strcmp(into, "/") ? "/" : "", base);
    if (moving && !strcmp(here, from)) // already there
        return NULL;
    if (!strncmp(into, from, length) && (into[length] == '/' || !into[length])) {
        snprintf(status, sizeof(status), "Can't put %s inside itself", base);
        return NULL;
    }
    free_name(into, base, name);
    snprintf(to, sizeof(to), "%s%s%s", into, strcmp(into, "/") ? "/" : "", name);
    if (moving)
        ok = rename(from, to) == 0 || (errno == EXDEV && gui_copy_tree(from, to) && gui_delete_tree(from));
    else
        ok = gui_copy_tree(from, to);
    if (!ok) {
        snprintf(status, sizeof(status), "Can't put %s in %s: %s", base, into, strerror(errno));
        return NULL;
    }
    printf("files: %s %s to %s\n", done, from, to);
    snprintf(status, sizeof(status), "%c%s %s", toupper((unsigned char)done[0]), done + 1, base);
    return to;
}

// Copies what's on the clipboard here, a path a line, or moves it if it was cut.
static void paste(void) {
    static char cut[MAX_ENTRIES * 64], moved[MAX_ENTRIES * 64];
    char *from = gui_clipboard_get(NULL), last[PATH_MAX] = "";
    FILE *f = fopen(CUT, "r");
    size_t n = 0;

    if (f) {
        n = fread(cut, 1, sizeof(cut) - 1, f);
        fclose(f);
    }
    cut[n] = '\0';
    if (!from || from[0] != '/') {
        snprintf(status, sizeof(status), "There's nothing to paste");
        free(from);
        return;
    }

    bool moving = !strcmp(cut, from);
    size_t used = 0;

    for (char *line = strtok(from, "\n"); line; line = strtok(NULL, "\n")) {
        const char *to;

        if (access(line, F_OK) == 0 && (to = put(line, folder, moving, moving ? "moved" : "pasted"))) {
            snprintf(last, sizeof(last), "%s", to);
            used += snprintf(moved + used, sizeof(moved) - used, "%s%s", used ? "\n" : "", to);
            used = used < sizeof(moved) ? used : sizeof(moved) - 1;
        }
    }
    if (moving) { // where they are now, to paste copies of
        unlink(CUT);
        gui_clipboard_set(moved, used);
    }
    read_folder(folder, last[0] ? strrchr(last, '/') + 1 : NULL);
    free(from);
}

// A file dropped on a folder, or a tab, goes there, and on the rest of the window
// it comes here, if it isn't here already. It's moved, or copied with Ctrl held.
static void take_drop(struct gui_ui *ui) {
    char into[PATH_MAX], from_folder[PATH_MAX];

    snprintf(from_folder, sizeof(from_folder), "%.*s", (int)(strrchr(dropped, '/') - dropped), dropped);
    if (ui->drop_tab >= 0)
        snprintf(into, sizeof(into), "%s", tabs[ui->drop_tab].folder);
    else if (ui->drop_item >= 0 && entries[ui->drop_item].folder)
        path_of(into, entries[ui->drop_item].name);
    else
        snprintf(into, sizeof(into), "%s", folder);
    if ((!strcmp(into, folder) && !strcmp(from_folder[0] ? from_folder : "/", folder) && !drop_copies)
        || !strcmp(into, dropped))
        printf("files: dropped %s where it was\n", dropped);
    else if (!strcmp(into, GUI_TRASH) ? gui_trash(dropped) : put(dropped, into, !drop_copies, drop_copies ? "copied" : "moved") != NULL)
        refresh();
    dropped[0] = '\0';
}

// Moves what's chosen to the trash, or deletes it for good, or in the trash puts
// it back where it was.
enum { TRASH, DELETE, RESTORE };
static void dispose(int how) {
    int picked[MAX_ENTRIES], n = chosen_items(picked), done = 0;

    for (int i = 0; i < n; i++) {
        const struct entry *e = &entries[picked[i]];
        char path[PATH_MAX], to[GUI_PATH_MAX];

        path_of(path, e->name);

        bool ok = how == TRASH     ? gui_trash(path)
                : how == RESTORE ? gui_trash_restore(e->name, to)
                : in_trash()     ? gui_trash_delete(e->name)
                                 : gui_delete_tree(path);

        if (!ok) {
            snprintf(status, sizeof(status), "Can't %s %s: %s", how == RESTORE ? "restore" : "delete", e->shown,
                     strerror(errno));
            break;
        }
        printf("files: %s %s\n", how == TRASH ? "trashed" : how == RESTORE ? "restored" : "deleted",
               how == RESTORE ? to : in_trash() ? e->shown : path);
        done++;
    }
    asking = NOTHING;
    if (done == n)
        read_folder(folder, NULL);
}

// Opens what's chosen in the app the strip was given.
static void answered(void) {
    asking = NOTHING;
    if (app_count)
        open_in(apps[app_chosen]);
}

// The rename, once it's said to be done, if the name's changed.
static void renamed(void) {
    char from[PATH_MAX], to[PATH_MAX];

    if (!strcmp(new_name, renaming) || !new_name[0])
        ;
    else if (strchr(new_name, '/'))
        snprintf(status, sizeof(status), "A name can't have a / in it");
    else if (path_of(to, new_name), !access(to, F_OK))
        snprintf(status, sizeof(status), "There's already something called %s", new_name);
    else if (path_of(from, renaming), rename(from, to) < 0)
        snprintf(status, sizeof(status), "Can't rename %s: %s", renaming, strerror(errno));
    else {
        printf("files: renamed %s to %s\n", from, to);
        renaming[0] = '\0';
        read_folder(folder, new_name);
        return;
    }
    renaming[0] = '\0';
}

// A new folder or empty file, called what's free of "New folder", "New folder 2" and
// on, then renamed in place, as Windows does.
static void make_new(bool is_folder) {
    char name[NAME_MAX + 1], path[PATH_MAX];
    int fd;

    for (int n = 1; n < 1000; n++) {
        if (is_folder)
            snprintf(name, sizeof(name), n > 1 ? "New folder %d" : "New folder", n);
        else
            snprintf(name, sizeof(name), n > 1 ? "New file %d.txt" : "New file.txt", n);
        path_of(path, name);
        if (is_folder ? mkdir(path, 0755) == 0 : (fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) >= 0 && close(fd) == 0) {
            printf("files: made %s\n", path);
            read_folder(folder, name);
            start_renaming();
            return;
        }
        if (errno != EEXIST)
            break;
    }
    snprintf(status, sizeof(status), "Can't make a new %s: %s", is_folder ? "folder" : "file", strerror(errno));
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[11];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("files: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void address_bar(struct gui_ui *ui) {
    if (gui_button(ui, "Up"))
        go_up();
    report(ui, 0, "up");
    gui_same_line(ui);
    if (gui_button(ui, "Go"))
        read_folder(typed, NULL);
    gui_same_line(ui);
    if (gui_button(ui, grid ? "List" : "Grid"))
        set_grid(!grid);
    report(ui, 3, "view");
    gui_same_line(ui);
    gui_column_begin(ui, gui_room(ui) - 180);
    gui_textbox(ui, ADDRESS, typed, sizeof(typed));
    report(ui, 1, "address");
    gui_column_end(ui);
    if (gui_focused(ui, ADDRESS) && gui_key(ui, KEY_ENTER))
        read_folder(typed, NULL);

    // Searching lists only what has the words in its name.
    if (gui_shortcut(ui, 'f'))
        gui_set_focus(ui, SEARCH, search);
    if (gui_textbox(ui, SEARCH, search, sizeof(search))) {
        refresh();
        printf("files: searching for %s\n", search);
    }
}

static void set_sort(int by) {
    static const char *const names[] = {"name", "size", "date"};

    sort_by = by;
    refresh();
    printf("files: sorted by %s\n", names[by]);
}

static void toggle_hidden(void) {
    show_hidden = !show_hidden;
    refresh();
    printf("files: hidden files %s\n", show_hidden ? "shown" : "hidden");
}

// The drive the folder shown is on, or -1.
static int drive_shown(void) {
    for (int i = 0; i < drive_count; i++) {
        size_t n = strlen(drives[i].mount);

        if (n && !strncmp(folder, drives[i].mount, n) && (!folder[n] || folder[n] == '/'))
            return i;
    }
    return -1;
}

// A drive opened from the side: mounted, if it isn't, and shown.
static void open_drive(int i) {
    char error[256];

    remember();
    if (!drive_mount(&drives[i], error, sizeof(error))) {
        snprintf(status, sizeof(status), "Can't open %s: %s", drives[i].name, error);
        printf("files: can't mount %s: %s\n", drives[i].name, error);
        return;
    }
    printf("files: mounted %s at %s\n", drives[i].name, drives[i].mount);
    read_folder(drives[i].mount, NULL);
}

// The drive shown unmounted and powered off, so it can be taken out, and home shown.
static void eject_drive(int i) {
    struct drive d = drives[i];
    char error[256];

    if (!drive_eject(&d, error, sizeof(error))) {
        snprintf(status, sizeof(status), "Can't eject %s: %s", d.name, error);
        printf("files: can't eject %s: %s\n", d.name, error);
        return;
    }
    read_folder(place_paths[0], NULL);
    snprintf(status, sizeof(status), "%s can be taken out", d.name);
    printf("files: ejected %s\n", d.name);
}

// Says where each drive is down the side, for the tests, when they change.
static void report_drives(struct gui_rect list) {
    static char said[DRIVES_MAX * 80 + 64];
    char now[sizeof(said)];
    int n = snprintf(now, sizeof(now), "files: drives:");

    for (int i = 0; i < drive_count; i++)
        n += snprintf(now + n, sizeof(now) - n, " %s at %d %d %d %d%s;", drives[i].name, list.x,
                      list.y + 2 + (PLACE_COUNT + i) * (GUI_CHAR_HEIGHT + 6), list.width, GUI_CHAR_HEIGHT + 6,
                      drives[i].mount[0] ? " mounted" : "");
    if (strcmp(now, said))
        printf("%s\n", now);
    snprintf(said, sizeof(said), "%s", now);
}

// Places down the side, then the drives plugged in, the one this folder's in
// highlighted, and how the folder's listed under them.
static void sidebar(struct gui_ui *ui) {
    static const char *const sorts[] = {"By name", "By size", "By date"};
    int by = sort_by, count = PLACE_COUNT + drive_count, on = drive_shown();
    bool hidden = show_hidden;
    const char *names[PLACE_COUNT + DRIVES_MAX];
    const struct gui_image *place_icons[PLACE_COUNT + DRIVES_MAX];
    int at = on >= 0 ? PLACE_COUNT + on : -1;

    for (int i = 0; i < PLACE_COUNT; i++) {
        names[i] = place_names[i];
        place_icons[i] = gui_file_icon("", true);
        if (at < 0 && !strcmp(place_paths[i], folder))
            at = i;
    }
    for (int i = 0; i < drive_count; i++) {
        names[PLACE_COUNT + i] = drives[i].name;
        place_icons[PLACE_COUNT + i] = gui_icon("drive");
    }
    gui_column_begin(ui, SIDEBAR);
    if (gui_list_icons(ui, PLACES, names, place_icons, count, &at, count) && at >= 0) {
        if (at >= PLACE_COUNT) {
            open_drive(at - PLACE_COUNT);
        } else {
            remember();
            if (strcmp(place_paths[at], RECENT))
                mkdir(place_paths[at], 0755); // the trash, if nothing's been put there yet
            read_folder(place_paths[at], NULL);
        }
    }
    report_drives(gui_last_rect(ui));
    if (on >= 0) {
        if (gui_button(ui, "Eject"))
            eject_drive(on);
        report(ui, 9, "eject");
    }
    if (gui_dropdown(ui, "##sort", sorts, 3, &by))
        set_sort(by);
    report(ui, 4, "sort");
    if (gui_checkbox(ui, "Hidden", &hidden))
        toggle_hidden();
    report(ui, 5, "hidden");
    gui_column_end(ui);
}

// Asks for the app to open something with, under the address bar.
static void asking_strip(struct gui_ui *ui) {
    bool yes, no;

    if (ask_focus)
        gui_set_focus(ui, "##app", NULL);
    ask_focus = false;
    gui_label(ui, "Open with:");
    gui_same_line(ui);
    yes = gui_button(ui, "Open");
    report(ui, 8, "ok");
    gui_same_line(ui);
    no = gui_button(ui, "Cancel");
    gui_same_line(ui);
    gui_dropdown(ui, "##app", apps, app_count, &app_chosen);
    report(ui, 6, "app");
    yes |= gui_focused(ui, "##app") && gui_key(ui, KEY_ENTER) && !ui->popup;
    if (no || gui_key(ui, KEY_ESCAPE))
        asking = NOTHING;
    else if (yes)
        answered();
}

// How much is in a folder, all the way down, and how many files.
static off_t size_of_tree(const char *path, int *files) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    off_t total = 0;

    if (lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode)) {
        ++*files;
        return st.st_size;
    }
    if (!(dir = opendir(path)))
        return 0;
    while ((d = readdir(dir))) {
        char inside[PATH_MAX];

        if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
            snprintf(inside, sizeof(inside), "%s/%s", path, d->d_name);
            total += size_of_tree(inside, files);
        }
    }
    closedir(dir);
    return total;
}

static void human_size(char *out, size_t n, off_t size) {
    if (size < 1024)
        snprintf(out, n, "%lld bytes", (long long)size);
    else if (size < 1024 * 1024)
        snprintf(out, n, "%.1f kB (%lld bytes)", size / 1024.0, (long long)size);
    else
        snprintf(out, n, "%.1f MB (%lld bytes)", size / (1024.0 * 1024), (long long)size);
}

// What's known about the selected item, worked out once when it's asked for. In
// the trash, that's where it came from.
static void describe_properties(char *out, size_t n) {
    const struct entry *e = chosen();
    char path[PATH_MAX], size[64], when[32], kind[64], from[GUI_PATH_MAX];
    struct stat st;
    int files = 0;

    path_of(path, e->name);
    if (stat(path, &st) < 0) {
        snprintf(out, n, "Can't look at %s: %s", e->shown, strerror(errno));
        return;
    }
    human_size(size, sizeof(size), e->folder ? size_of_tree(path, &files) : st.st_size);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
    if (e->folder)
        snprintf(kind, sizeof(kind), "Folder of %d files", files);
    else
        snprintf(kind, sizeof(kind), "%s", gui_is_program(path)                 ? "Program"
                                         : icons[selected] == gui_icon("image") ? "Picture"
                                         : icons[selected] == gui_icon("audio") ? "Sound"
                                                                                : "File");
    if (!in_trash() || gui_trash_origin(e->name, from)) {
        snprintf(from, sizeof(from), "%s", in_trash() ? from : path);
        *strrchr(from, '/') = '\0';
    } else {
        snprintf(from, sizeof(from), "%s", folder);
    }
    snprintf(out, n, "Name:      %s\nKind:      %s\nSize:      %s\nChanged:   %s\n%s%s\nMode:      %c%c%c%c%c%c%c%c%c",
             e->shown, kind, size, when, in_trash() ? "From:      " : "In:        ", from[0] ? from : "/",
             st.st_mode & 0400 ? 'r' : '-', st.st_mode & 0200 ? 'w' : '-', st.st_mode & 0100 ? 'x' : '-',
             st.st_mode & 040 ? 'r' : '-', st.st_mode & 020 ? 'w' : '-', st.st_mode & 010 ? 'x' : '-',
             st.st_mode & 04 ? 'r' : '-', st.st_mode & 02 ? 'w' : '-', st.st_mode & 01 ? 'x' : '-');
    printf("files: properties of %s: %s\n", path, size);
}

static bool dialog_open(void) {
    return asking == DELETING || asking == EMPTYING || asking == PROPERTIES;
}

// Deleting for good, emptying the trash and properties are dialogs over the window.
static void dialogs(struct gui_ui *ui) {
    static const char *const delete_buttons[] = {"Delete", "Cancel"}, *const empty_buttons[] = {"Empty", "Cancel"},
                             *const ok[] = {"OK"};
    static char text[PATH_MAX + 512];
    static int shown = NOTHING;
    int picked[MAX_ENTRIES], n = chosen_items(picked);

    if (!dialog_open()) {
        shown = NOTHING;
        return;
    }
    if (shown != (int)asking && asking == PROPERTIES)
        describe_properties(text, sizeof(text));
    else if (shown != (int)asking && asking == EMPTYING)
        snprintf(text, sizeof(text), "Delete everything in the trash?\nIt can't be undone.");
    else if (shown != (int)asking && n > 1)
        snprintf(text, sizeof(text), "Delete these %d items for good?\nIt can't be undone.", n);
    else if (shown != (int)asking)
        snprintf(text, sizeof(text), "Delete %s%s for good?\nIt can't be undone.", chosen()->shown,
                 chosen()->folder ? " and everything in it" : "");
    if (shown != (int)asking)
        printf("files: asking to %s\n", asking == EMPTYING ? "empty the trash" : asking == DELETING ? "delete" : "show properties");
    shown = asking;

    int button = asking == PROPERTIES ? gui_dialog(ui, text, ok, 1)
                                      : gui_dialog(ui, text, asking == EMPTYING ? empty_buttons : delete_buttons, 2);

    if (button == 1 && asking == DELETING) {
        dispose(DELETE);
    } else if (button == 1 && asking == EMPTYING) {
        if (gui_trash_empty())
            printf("files: emptied the trash\n");
        read_folder(folder, NULL);
    } else if (button) {
        asking = NOTHING;
    }
}

// Right-clicking an item has what can be done with it, and anywhere else in the
// list what can be done in the folder. In the trash, things can be put back.
static void context_menu(struct gui_ui *ui, struct gui_rect list) {
    if (!gui_context_menu(ui, "##menu", list))
        return;

    const struct entry *e = ui->clicked_item >= 0 ? chosen() : NULL;
    char path[PATH_MAX];

    if (e && in_trash()) {
        if (gui_menu_icon_item(ui, "Restore", gui_icon("undo")))
            dispose(RESTORE);
        if (gui_menu_icon_item(ui, "Delete for good", gui_icon("delete")))
            ask_about(DELETING);
        gui_menu_separator(ui);
        if (gui_menu_icon_item(ui, "Properties", gui_icon("info")))
            ask_about(PROPERTIES);
    } else if (e) {
        path_of(path, e->name);
        if (gui_menu_icon_item(ui, "Open", gui_icon("open")))
            open_selected();
        if (e->folder && gui_menu_icon_item(ui, "Open in new tab", gui_icon("add")))
            new_tab(path);
        if (!e->folder && gui_menu_icon_item(ui, "Open in Editor", gui_icon("apps/editor")))
            open_in("editor");
        if (!e->folder && gui_menu_icon_item(ui, "Open with...", gui_icon("open")))
            ask_about(OPENING_WITH);
        if (!e->folder && gui_is_program(path) && gui_menu_icon_item(ui, "Run", gui_icon("apps/run")))
            run_selected();
        gui_menu_separator(ui);
        if (gui_menu_icon_item(ui, "Cut", gui_icon("cut")))
            copy_selected(true);
        if (gui_menu_icon_item(ui, "Copy", gui_icon("copy")))
            copy_selected(false);
        if (gui_menu_icon_item(ui, "Copy path", gui_icon("link")))
            copy_path();
        if (can_rename() && gui_menu_icon_item(ui, "Rename", gui_icon("rename")))
            ask_about(RENAMING);
        if (gui_menu_icon_item(ui, "Delete", gui_icon("delete")))
            dispose(TRASH);
        gui_menu_separator(ui);
        if (gui_menu_icon_item(ui, "Properties", gui_icon("info")))
            ask_about(PROPERTIES);
    } else if (in_trash()) {
        if (gui_menu_icon_item(ui, "Empty trash", gui_icon("delete")))
            ask_about(EMPTYING);
        if (gui_menu_icon_item(ui, "Refresh", gui_icon("refresh")))
            refresh();
    } else if (in_recent()) {
        if (gui_menu_icon_item(ui, "Clear recent", gui_icon("delete"))) {
            gui_recent_clear();
            printf("files: cleared recent\n");
            refresh();
        }
        if (gui_menu_icon_item(ui, "Refresh", gui_icon("refresh")))
            refresh();
    } else {
        if (gui_menu_icon_item(ui, "New folder", gui_icon("new-folder")))
            make_new(true);
        if (gui_menu_icon_item(ui, "New file", gui_icon("new-file")))
            make_new(false);
        if (gui_menu_icon_item(ui, "Paste", gui_icon("clipboard")))
            paste();
        gui_menu_separator(ui);
        if (gui_menu_icon_item(ui, "Open terminal here", gui_icon("apps/term")))
            start_app("term", NULL, folder);
        if (gui_menu_icon_item(ui, "Refresh", gui_icon("refresh")))
            refresh();
    }
    gui_menu_end(ui);
}

// Keys for the list, while it has the keyboard. Delete moves what's chosen to the
// trash, and with Shift, or in the trash, deletes it for good, once that's agreed to.
static void list_keys(struct gui_ui *ui) {
    bool shift = false;

    for (int i = 0; i < ui->key_count; i++)
        shift |= ui->keys[i].code == KEY_DELETE && (ui->keys[i].modifiers & KEY_MOD_SHIFT);
    if (!gui_focused(ui, LIST) || asking || renaming[0])
        return;
    if (gui_key(ui, KEY_BACKSPACE))
        go_up();
    else if (gui_key(ui, KEY_F5))
        refresh();
    else if (chosen() && gui_key(ui, KEY_F2) && !in_trash() && can_rename())
        ask_about(RENAMING);
    else if (chosen() && gui_key(ui, KEY_DELETE) && (shift || in_trash()))
        ask_about(DELETING);
    else if (chosen() && gui_key(ui, KEY_DELETE))
        dispose(TRASH);
    else if (chosen() && (gui_shortcut(ui, 'c') || gui_shortcut(ui, 'x')) && !in_trash())
        copy_selected(gui_shortcut(ui, 'x'));
    else if (gui_shortcut(ui, 'v') && !in_trash() && !in_recent())
        paste();
}

// Dragging an item takes it to another window, through wm, to open it there.
static void drag(struct gui_ui *ui, struct gui_rect list) {
    static bool may_drag;
    static int from_x, from_y;
    char path[PATH_MAX];

    if (ui->mouse_pressed && ui->clicked_item >= 0 && ui->mouse_x >= list.x && ui->mouse_y >= list.y
        && ui->mouse_x < list.x + list.width && ui->mouse_y < list.y + list.height) {
        may_drag = true;
        from_x = ui->mouse_x;
        from_y = ui->mouse_y;
    }
    if (!ui->mouse_down)
        may_drag = false;
    if (!may_drag || !chosen() || abs(ui->mouse_x - from_x) + abs(ui->mouse_y - from_y) < DRAG_DISTANCE)
        return;
    may_drag = false;
    path_of(path, chosen()->name);
    if (gui_drag_file(path))
        printf("files: dragging %s\n", path);
}

static void frame(struct gui_ui *ui) {
    int active = current, closing = 0, result;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ; // programs it started that have finished
    gui_ui_begin(ui);
    if (dialog_open())
        dialogs(ui);
    else
        gui_help(ui, help);
    if (gui_shortcut(ui, 'h') && !renaming[0])
        toggle_hidden();
    result = gui_tabs(ui, tab_names, tab_count, &active, &closing);
    if (result == GUI_TABS_CHANGED) {
        remember();
        show_tab(active);
    } else if (result == GUI_TABS_NEW)
        new_tab(folder);
    else if (result == GUI_TABS_CLOSE)
        close_tab(closing);
    address_bar(ui);
    if (asking && !dialog_open())
        asking_strip(ui);
    if (in_trash()) {
        if (gui_button(ui, "Empty trash") && count)
            ask_about(EMPTYING);
        report(ui, 7, "empty");
        gui_same_line(ui);
    }
    gui_label(ui, "%s", status);
    sidebar(ui);

    gui_list_marks(ui, marked);
    for (int i = 0; renaming[0] && i <= count; i++) { // where it is now, as the list can change
        if (i == count)
            renaming[0] = '\0';
        else if (!strcmp(entries[i].name, renaming)) {
            if (rename_started) {
                gui_edit_name(ui, new_name, entries[i].folder);
                gui_set_focus(ui, LIST, NULL);
                rename_started = false;
            }
            gui_list_edit(ui, i, new_name, sizeof(new_name));
            break;
        }
    }
    result = grid ? gui_grid(ui, LIST, items, pictures, count, &selected)
                  : gui_list_icons(ui, LIST, items, icons, count, &selected, 0);
    if (renaming[0]) {
        int edited = gui_edit_result(ui);

        if (edited == GUI_EDIT_DONE)
            renamed();
        else if (edited == GUI_EDIT_CANCELLED)
            renaming[0] = '\0';
        if (edited)
            gui_post(&(struct gui_message){.type = GUI_REDRAW});
    }

    struct gui_rect list = gui_last_rect(ui);

    report(ui, 2, "list");
    drag(ui, list);
    if (result == GUI_LIST_ACTIVATED)
        open_selected();
    else if (result == GUI_LIST_CHANGED && asking == OPENING_WITH)
        asking = NOTHING; // they were about the one that was selected
    if (result)
        describe();
    if (ui->dropping && dropped[0])
        take_drop(ui);
    list_keys(ui);
    context_menu(ui, list);
    gui_ui_end(ui);
    fflush(stdout);
}

// Goes to the folder a file's in, with it selected, for the desktop to ask about it.
static bool select_file(const char *path, int what) {
    char in[PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (!slash)
        return false;
    snprintf(in, sizeof(in), "%.*s", (int)(slash - path), path);
    if (!read_folder(in[0] ? in : "/", slash + 1) || !count || strcmp(chosen()->name, slash + 1))
        return false;
    if (what != NOTHING)
        ask_about(what);
    return true;
}

// A window showing what's in folders, in tabs, to go about them and open what's there.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: files [folder | --select file | --rename file | --properties file]\n"
                            "Opens a window showing what's in the folder, or the home folder, or the\n"
                            "file's folder with it selected, or asks for a file's new name or shows its\n"
                            "properties.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "files: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Files", 560, 440);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "files: couldn't make a window\n");
        return 1;
    }
    tab_count = 1;
    tab_names[0] = tabs[0].name;
    const char *home = getenv("HOME");

    for (int i = 0; i < 4; i++)
        snprintf(place_paths[i], PATH_MAX, "%s%s%s", home ? home : "", i ? "/" : "", i ? place_names[i] : "");
    snprintf(place_paths[4], PATH_MAX, "/");
    snprintf(place_paths[5], PATH_MAX, "%s", GUI_TRASH);
    snprintf(place_paths[6], PATH_MAX, RECENT);
    app_count = gui_app_names(app_names, MAX_APPS);
    for (int i = 0; i < app_count; i++)
        apps[i] = app_names[i];

    const char *option = argc > 2 ? argv[1] : "";
    int what = !strcmp(option, "--rename") ? RENAMING : !strcmp(option, "--properties") ? PROPERTIES : NOTHING;
    bool asked = what != NOTHING || !strcmp(option, "--select");

    if (asked && !select_file(argv[2], what))
        dprintf(STDERR_FILENO, "files: there's no %s\n", argv[2]);
    else if (!asked && !read_folder(argc > 1 ? argv[1] : home ? home : "/", NULL))
        read_folder("/", NULL);
    drives_open();
    gui_ui_init(&ui, window);
    frame(&ui);
    // The window's events, and udisks2's, as drives come and go.
    while (!quitting) {
        int wait, due = gui_due_ms();
        struct pollfd fds[2] = {{.fd = gui_fd(), .events = POLLIN}, drives_poll(&wait)};

        wait = gui_ready() ? 0 : due >= 0 && (wait < 0 || due < wait) ? due : wait;
        poll(fds, 2, wait);
        if (drives_read()) {
            if (drive_shown() < 0 && strncmp(folder, "/run/media/", 11) == 0)
                read_folder(place_paths[0], NULL); // the drive shown went
            frame(&ui);
        }

        int got = gui_wait_event(&e, 0);

        if (got < 0 || (got && e.type == GUI_CLOSE))
            break;
        if (!got)
            continue;
        if (gui_dropped_file(&e, dropped)) {
            drop_copies = e.key.modifiers & KEY_MOD_CTRL;
            gui_ui_drop(&ui, e.x, e.y);
            frame(&ui);
        }
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
