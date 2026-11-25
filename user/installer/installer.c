#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

// Installs AximOS from the live system, through aximos-install, which it runs as
// root with sudo: it asks which drive, and how, then for the first account, the
// computer's name and the time zone, and shows how far it's got.

#define MAX_DRIVES 16

enum page { CHOOSE, ACCOUNT, ZONE, CONFIRM, INSTALLING, DONE, FAILED };
enum way { REPLACE, FREE, WHOLE, WAYS }; // the kindest to what's on the drive first

struct drive {
    char name[32], size[24], free[24];
    bool can[WAYS];
};

static const char *const page_names[] = {"choose", "account", "zone", "confirm", "installing", "done", "failed"};
static const char *const way_names[] = {"replace", "free", "whole"};
static const char *const way_text[] = {"Replace the AximOS on it", "In its free space, beside what's there",
                                       "Over the whole drive, erasing it"};
static struct drive drives[MAX_DRIVES];
static char labels[MAX_DRIVES][96], output[8192];
static const char *drive_items[MAX_DRIVES], *way_items[WAYS];
static enum way ways[WAYS];
static int drive_count, way_count, chosen, way;
static enum page page;
static char name[33], password[128], again[128], hostname[64] = "aximos", message[160];
static bool dev_tools;
static int percent;
static pid_t child = -1;
static int from_child = -1;

static char **zones;
static const char **shown; // the zones the search matches
static int zone_count, shown_count, zone_at;
static char search[64], zone[GUI_PATH_MAX] = "UTC";

static void find_drives(void) {
    FILE *f = popen("sudo -n aximos-install list", "r");
    char line[256];

    while (f && drive_count < MAX_DRIVES && fgets(line, sizeof(line), f)) {
        struct drive *d = &drives[drive_count];
        char *drive = strtok(line, "\t"), *size = strtok(NULL, "\t"), *free = strtok(NULL, "\t"), *can = strtok(NULL, "\n");

        if (!can || !*can)
            continue;
        snprintf(d->name, sizeof(d->name), "%s", drive);
        snprintf(d->size, sizeof(d->size), "%s", size);
        snprintf(d->free, sizeof(d->free), "%s", free);
        for (int w = 0; w < WAYS; w++)
            d->can[w] = strstr(can, way_names[w]);
        snprintf(labels[drive_count], sizeof(labels[0]), "%s, %s%s%s", d->name, d->size,
                 d->can[REPLACE] ? ", with AximOS on it" : "", d->can[FREE] && !d->can[REPLACE] ? ", with room free" : "");
        drive_items[drive_count] = labels[drive_count];
        drive_count++;
    }
    if (f)
        pclose(f);
}

static void find_ways(void) {
    way_count = 0;
    for (int w = 0; drive_count && w < WAYS; w++) {
        if (drives[chosen].can[w]) {
            ways[way_count] = w;
            way_items[way_count++] = way_text[w];
        }
    }
    way = 0;
}

static bool contains(const char *text, const char *part) {
    size_t n = strlen(part);

    for (; *text; text++) {
        if (!strncasecmp(text, part, n))
            return true;
    }
    return !n;
}

// The zones the search matches, keeping the chosen one chosen if it still is, or
// else choosing the first.
static void filter(void) {
    shown_count = zone_at = 0;
    for (int i = 0; i < zone_count; i++) {
        if (contains(zones[i], search)) {
            if (!strcmp(zones[i], zone))
                zone_at = shown_count;
            shown[shown_count++] = zones[i];
        }
    }
    if (shown_count && strcmp(shown[zone_at], zone))
        snprintf(zone, sizeof(zone), "%s", shown[zone_at]);
}

static void go_to(struct gui_ui *ui, enum page to) {
    static const char *const focus[] = {"##drive", "##name", "##search", "Install", "##none", "Restart", "Back"};

    page = to;
    message[0] = '\0';
    gui_set_focus(ui, focus[page], NULL);
    ui->again = true;
    printf("installer: %s\n", page_names[page]);
    fflush(stdout);
}

static void fail(struct gui_ui *ui, const char *what) {
    go_to(ui, FAILED);
    snprintf(message, sizeof(message), "%s", what);
    printf("installer: failed: %s\n", what);
}

// aximos-install, as root, given the account's password on its standard input.
static void start(struct gui_ui *ui) {
    int in[2], out[2];

    if (pipe(in) < 0 || pipe(out) < 0 || (child = fork()) < 0)
        return fail(ui, "The installer couldn't be started.");
    if (child == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[1]);
        close(out[0]);
        char *argv[] = {"sudo", "-n", "aximos-install", drives[chosen].name, (char *)way_names[ways[way]], "--name", name,
                        "--hostname", hostname, "--timezone", zone, dev_tools ? "--dev" : NULL, NULL};
        execvp("sudo", argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    dprintf(in[1], "%s\n", password);
    close(in[1]);
    memset(password, 0, sizeof(password));
    memset(again, 0, sizeof(again));
    from_child = out[0];
    fcntl(from_child, F_SETFL, O_NONBLOCK);
    output[0] = '\0';
    percent = 0;
    go_to(ui, INSTALLING);
}

// What install said last, but for how far it's copied, up to count lines, the
// last one first.
static int last_lines(char lines[][96], int count) {
    int n = 0;
    char *end = output + strlen(output);

    while (n < count && end > output) {
        char *start = end - 1;

        while (start > output && start[-1] != '\n')
            start--;
        if (end - start > 1 && strncmp(start, "aximos-install: copied ", 23))
            snprintf(lines[n++], 96, "%.*s", (int)(end - start - (end[-1] == '\n')), start);
        end = start;
    }
    return n;
}

// What install has said since, until it's finished.
static void follow(struct gui_ui *ui) {
    size_t have = strlen(output);
    ssize_t n;
    int status;

    while ((n = read(from_child, output + have, sizeof(output) - 1 - have)) > 0) {
        output[have + n] = '\0';
        fputs(output + have, stdout);
        have += n;
        if (have > sizeof(output) / 2) { // keep the end
            memmove(output, output + have - sizeof(output) / 4, sizeof(output) / 4 + 1);
            have = strlen(output);
        }
    }
    fflush(stdout);
    for (char *p = output; (p = strstr(p, "aximos-install: copied ")); p++)
        percent = atoi(p + 23);
    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        waitpid(child, &status, 0);
        close(from_child);
        from_child = -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && strstr(output, "aximos-install: done"))
            go_to(ui, DONE);
        else
            fail(ui, strstr(output, "sudo:") ? "It needs to run as root, which this account can't without a password."
                                              : "It couldn't be installed. This is what install said last:");
    }
}

static void choose(struct gui_ui *ui) {
    bool next = gui_key(ui, KEY_ENTER);

    gui_heading(ui, "Install AximOS");
    if (!drive_count) {
        gui_label(ui, "There's no drive it can go on. It needs one of 16 GiB, or that much free.");
        return;
    }
    gui_label(ui, "Drive");
    if (gui_dropdown(ui, "##drive", drive_items, drive_count, &chosen))
        find_ways();
    gui_label(ui, "How");
    gui_dropdown(ui, "##how", way_items, way_count, &way);
    gui_checkbox(ui, "Development tools, to build programs (base-devel and git)", &dev_tools);
    if (gui_button(ui, "Next") || next)
        go_to(ui, ACCOUNT);
}

static bool hostname_ok(const char *s) {
    if (!*s || *s == '-' || strlen(s) > 63)
        return false;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-'))
            return false;
    }
    return true;
}

static void account(struct gui_ui *ui) {
    bool next = gui_key(ui, KEY_ENTER);

    gui_heading(ui, "Your account");
    gui_label(ui, "The first account on the installed AximOS, which can use sudo.");
    gui_label(ui, "Username");
    gui_textbox(ui, "##name", name, sizeof(name));
    gui_label(ui, "Password");
    gui_password(ui, "##password", password, sizeof(password));
    gui_label(ui, "Password again");
    gui_password(ui, "##again", again, sizeof(again));
    gui_label(ui, "The computer's name");
    gui_textbox(ui, "##hostname", hostname, sizeof(hostname));
    if (message[0])
        gui_label(ui, "%s", message);
    if (gui_button(ui, "Back"))
        return go_to(ui, CHOOSE);
    gui_same_line(ui);
    if (!gui_button(ui, "Next") && !next)
        return;

    const char *problem = !account_name_ok(name) || name[0] < 'a' || name[0] > 'z'
                            ? "A username starts with a lowercase letter, then letters, digits, dots, dashes and underscores."
                        : !password[0]            ? "Choose a password."
                        : strcmp(password, again) ? "The passwords aren't the same."
                        : !hostname_ok(hostname)  ? "A computer's name is letters, digits and dashes."
                                                  : NULL;

    if (problem) {
        snprintf(message, sizeof(message), "%s", problem);
        printf("installer: %s\n", problem);
        if (strcmp(password, again))
            password[0] = again[0] = '\0';
        return;
    }
    go_to(ui, ZONE);
}

static void time_zone(struct gui_ui *ui) {
    bool enter = gui_key(ui, KEY_ENTER);

    gui_heading(ui, "Time zone");
    gui_label(ui, "%s, or search for another", zone);
    if (gui_textbox(ui, "##search", search, sizeof(search)))
        filter();
    if (gui_button(ui, "Back"))
        return go_to(ui, ACCOUNT);
    gui_same_line(ui);
    bool next = gui_button(ui, "Next");
    int picked = gui_list(ui, "##zones", shown, shown_count, &zone_at, 0);

    if (picked && shown_count)
        snprintf(zone, sizeof(zone), "%s", shown[zone_at]);
    if (next || enter || picked == GUI_LIST_ACTIVATED)
        go_to(ui, CONFIRM);
}

static void confirm(struct gui_ui *ui) {
    struct drive *d = &drives[chosen];
    enum way w = ways[way];
    bool go = gui_key(ui, KEY_ENTER);

    gui_heading(ui, w == FREE ? "Install?" : "Erase and install?");
    gui_label(ui, w == FREE    ? "AximOS goes in the %s free on %s, and what's there stays."
                  : w == WHOLE ? "Everything on %.0s%s is erased, and AximOS installed in its place."
                               : "The AximOS on %.0s%s is erased, with its files, and installed again.",
              d->free, d->name);
    gui_label(ui, "%s, for %s, in %s%s.", hostname, name, zone, dev_tools ? ", with development tools" : "");
    gui_label(ui, "It starts first when the computer does, with a menu of the others.");
    if (gui_button(ui, "Back"))
        return go_to(ui, ZONE);
    gui_same_line(ui);
    if (gui_button(ui, "Install") || go)
        start(ui);
}

static void frame(struct gui_ui *ui) {
    static int drawn = -1;

    if ((int)page != drawn) { // for the tests, as it's what's on the screen
        printf("installer: drew %s\n", page_names[page]);
        fflush(stdout);
        drawn = page;
    }
    gui_ui_begin(ui);
    switch (page) {
    case CHOOSE:
        choose(ui);
        break;
    case ACCOUNT:
        account(ui);
        break;
    case ZONE:
        time_zone(ui);
        break;
    case CONFIRM:
        confirm(ui);
        break;
    case INSTALLING: {
        char last[1][96];

        gui_heading(ui, "Installing");
        gui_label(ui, percent ? "Copying the system" : "Making its partitions");
        gui_progress(ui, percent / 100.0f);
        if (last_lines(last, 1))
            gui_label(ui, "%s", last[0]);
        break;
    }
    case DONE:
        gui_heading(ui, "AximOS is installed");
        gui_label(ui, "Take out the USB stick, then restart.");
        if (gui_button(ui, "Restart"))
            execlp("systemctl", "systemctl", "reboot", NULL);
        break;
    case FAILED: {
        char last[4][96];
        int n = last_lines(last, 4);

        gui_heading(ui, "It wasn't installed");
        gui_label(ui, "%s", message);
        for (int i = n - 1; i >= 0; i--) // what install said last, oldest first
            gui_label(ui, "%s", last[i]);
        if (gui_button(ui, "Back"))
            go_to(ui, CHOOSE);
        break;
    }
    }
    gui_ui_end(ui);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: installer\n"
                            "Installs AximOS from the live system onto a drive, as aximos-install does, in a window.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "installer: needs the desktop\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Install AximOS", 640, 420);
    struct gui_ui ui;

    if (!window) {
        dprintf(STDERR_FILENO, "installer: couldn't make its window\n");
        return 1;
    }
    find_drives();
    find_ways();
    zone_count = gui_time_zones(&zones);
    shown = calloc(zone_count + 1, sizeof(*shown));
    if (shown)
        filter();
    printf("installer: %d drives\n", drive_count);
    gui_ui_init(&ui, window);
    go_to(&ui, CHOOSE);

    // While installing, it looks at what install says every so often, and doesn't
    // close, which would leave the drive half installed.
    for (bool redraw = true;;) {
        struct gui_message e;
        int got;

        if (redraw)
            frame(&ui);
        redraw = page == INSTALLING;
        if (redraw)
            follow(&ui);
        if ((got = gui_wait_event(&e, redraw ? 100 : -1)) < 0 || (got && e.type == GUI_CLOSE && page != INSTALLING))
            break;
        if (got)
            redraw |= gui_ui_event(&ui, &e);
    }
    return 0;
}
