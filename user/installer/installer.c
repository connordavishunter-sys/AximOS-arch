#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Installs AximOS from the live system, through aximos-install, which it runs
// as root with sudo: it asks which drive, and how, then for the first account, and
// shows how far it's got.

#define MAX_DRIVES 16

enum page { CHOOSE, ACCOUNT, CONFIRM, INSTALLING, DONE, FAILED };
enum way { FREE, WHOLE, AGAIN, AGAIN_ERASE, WAYS };

struct drive {
    char name[32], size[24], free[24];
    bool can[WAYS];
};

static const char *const page_names[] = {"choose", "account", "confirm", "installing", "done", "failed"};
static const char *const how[] = {"free", "whole", "again", "again"};
static struct drive drives[MAX_DRIVES];
static char labels[MAX_DRIVES][96], way_labels[WAYS][96], output[8192];
static const char *drive_items[MAX_DRIVES], *way_items[WAYS];
static enum way ways[WAYS];
static int drive_count, way_count, chosen, way;
static enum page page;
static char name[64], password[128], again[128], own[128], message[160];
static int percent;
static pid_t child;
static int to_child = -1, from_child = -1;
static bool gave_password, gave_again, dev_tools = true;

static void find_drives(void) {
    FILE *f = popen("aximos-install list", "r");
    char line[256];

    while (f && drive_count < MAX_DRIVES && fgets(line, sizeof(line), f)) {
        struct drive *d = &drives[drive_count];
        char *drive = strtok(line, "\t"), *size = strtok(NULL, "\t"), *free = strtok(NULL, "\t"), *can = strtok(NULL, "\n");

        if (!can || !*can)
            continue;
        snprintf(d->name, sizeof(d->name), "%s", drive);
        snprintf(d->size, sizeof(d->size), "%s", size);
        snprintf(d->free, sizeof(d->free), "%s", free);
        d->can[FREE] = strstr(can, "free");
        d->can[WHOLE] = strstr(can, "whole");
        d->can[AGAIN] = d->can[AGAIN_ERASE] = strstr(can, "again");
        snprintf(labels[drive_count], sizeof(labels[0]), "%s, %s%s%s%s", d->name, d->size,
                 d->can[FREE] ? ", " : "", d->can[FREE] ? d->free : "", d->can[FREE] ? " free" : "");
        drive_items[drive_count] = labels[drive_count];
        drive_count++;
    }
    if (f)
        pclose(f);
}

// The ways the chosen drive allows, the kindest to what's on it first.
static void find_ways(void) {
    struct drive *d = &drives[chosen];
    static const enum way order[] = {AGAIN, FREE, AGAIN_ERASE, WHOLE};

    way_count = 0;
    for (int i = 0; i < WAYS; i++) {
        enum way w = order[i];

        if (!d->can[w])
            continue;
        snprintf(way_labels[way_count], sizeof(way_labels[0]), "%s",
                 w == FREE    ? "In its free space, beside what's there"
                 : w == WHOLE ? "Over the whole drive, erasing it"
                 : w == AGAIN ? "Again, keeping its accounts and /home"
                              : "Again, erasing its accounts and /home");
        ways[way_count] = w;
        way_items[way_count] = way_labels[way_count];
        way_count++;
    }
    way = 0;
}

static bool fresh(void) {
    return ways[way] != AGAIN;
}

static void go_to(struct gui_ui *ui, enum page to) {
    page = to;
    message[0] = '\0';
    gui_set_focus(ui, page == CHOOSE ? "##drive" : page == ACCOUNT ? "##name" : page == CONFIRM ? "##own" : "##none", NULL);
    ui->again = true;
    printf("installer: %s\n", page_names[page]);
    fflush(stdout);
}

static void fail(struct gui_ui *ui, const char *what) {
    go_to(ui, FAILED);
    snprintf(message, sizeof(message), "%s", what);
    printf("installer: failed: %s\n", what);
}

// install, through sudo, which is given the password first, then install the new
// account's when it asks.
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
        char *argv[10] = {"sudo", "aximos-install", drives[chosen].name, (char *)how[ways[way]], "-y"};
        int n = 5;

        if (fresh())
            argv[n++] = name;
        if (ways[way] == AGAIN_ERASE)
            argv[n++] = "--erase";
        if (!dev_tools)
            argv[n++] = "--no-dev";
        execvp("sudo", argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    to_child = in[1];
    from_child = out[0];
    fcntl(from_child, F_SETFL, O_NONBLOCK);
    dprintf(to_child, "%s\n", own);
    memset(own, 0, sizeof(own));
    output[0] = '\0';
    percent = 0;
    gave_password = gave_again = false;
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
        if (end - start > 1 && strncmp(start, "aximos-install: copied ", 23) && strncmp(start, "Password for", 12))
            snprintf(lines[n++], 96, "%.*s", (int)(end - start - (end[-1] == '\n')), start);
        end = start;
    }
    return n;
}

// What install has said since, answering what it asks, until it's finished.
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
    if (!gave_password && strstr(output, "New password: ")) {
        dprintf(to_child, "%s\n", password);
        gave_password = true;
    }
    if (!gave_again && strstr(output, "Again: ")) {
        dprintf(to_child, "%s\n", password);
        gave_again = true;
    }
    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        waitpid(child, &status, 0);
        close(to_child);
        close(from_child);
        to_child = from_child = -1;
        memset(password, 0, sizeof(password));
        memset(again, 0, sizeof(again));
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && strstr(output, "aximos-install: done"))
            go_to(ui, DONE);
        else
            fail(ui, strstr(output, "sudo: wrong password") ? "That password isn't right."
                     : strstr(output, "sudo: only") ? "Your account isn't in the admin group, so it can't install."
                                                    : "It couldn't be installed. This is what install said last:");
    }
}

static void choose(struct gui_ui *ui) {
    bool next = gui_key(ui, KEY_ENTER);

    gui_heading(ui, "Install AximOS");
    if (!drive_count) {
        gui_label(ui, "There's no drive it can go on. It needs 4 GiB, or that much free.");
        return;
    }
    gui_label(ui, "Drive");
    if (gui_dropdown(ui, "##drive", drive_items, drive_count, &chosen))
        find_ways();
    gui_label(ui, "How");
    gui_dropdown(ui, "##how", way_items, way_count, &way);
    gui_checkbox(ui, "Development tools, to build programs", &dev_tools);
    if (gui_button(ui, "Next") || next)
        go_to(ui, fresh() ? ACCOUNT : CONFIRM);
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
    if (message[0])
        gui_label(ui, "%s", message);
    if (gui_button(ui, "Back"))
        return go_to(ui, CHOOSE);
    gui_same_line(ui);
    if (!gui_button(ui, "Next") && !next)
        return;

    const char *problem = !account_name_ok(name) ? "A username is letters, digits, dots, dashes and underscores."
                        : !password[0]           ? "Choose a password."
                        : strcmp(password, again)  ? "The passwords aren't the same."
                                                   : NULL;

    if (problem) {
        snprintf(message, sizeof(message), "%s", problem);
        printf("installer: %s\n", problem);
        password[0] = again[0] = '\0';
        gui_set_focus(ui, account_name_ok(name) ? "##password" : "##name", name);
        return;
    }
    go_to(ui, CONFIRM);
}

static void confirm(struct gui_ui *ui) {
    struct drive *d = &drives[chosen];
    bool go = gui_key(ui, KEY_ENTER);

    gui_heading(ui, ways[way] == WHOLE || ways[way] == AGAIN_ERASE ? "Erase and install?" : "Install?");
    gui_label(ui, ways[way] == FREE    ? "AximOS goes in the %s free on %s, and what's there stays."
                  : ways[way] == WHOLE ? "Everything on %.0s%s is erased, and AximOS installed in its place."
                  : ways[way] == AGAIN ? "AximOS on %.0s%s is replaced, keeping its accounts and /home."
                                       : "AximOS on %.0s%s is erased, with its files, and installed again.",
              d->free, d->name);
    gui_label(ui, "It starts first when the computer does, with a menu of the others.");
    gui_label(ui, "Your password, to install as root");
    gui_password(ui, "##own", own, sizeof(own));
    if (gui_button(ui, "Back"))
        return go_to(ui, fresh() ? ACCOUNT : CHOOSE);
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
            execlp("reboot", "reboot", NULL);
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
                            "Installs AximOS from the live system onto a drive, as install does, in a window.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "installer: needs the desktop\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Install AximOS", 600, 340);
    struct gui_ui ui;

    if (!window) {
        dprintf(STDERR_FILENO, "installer: couldn't make its window\n");
        return 1;
    }
    find_drives();
    find_ways();
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
        if ((got = gui_wait_event(&e, redraw ? 100 : -1)) < 0
            || (got && e.type == GUI_CLOSE && page != INSTALLING))
            break;
        if (got)
            redraw |= gui_ui_event(&ui, &e);
    }
    return 0;
}
