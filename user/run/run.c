#include <aximos.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY gui_own_path("run_history") // newest first, a command on each line
#define MAX_HISTORY 16
#define COMMAND "##command"
#define WIDTH 480
#define HEIGHT 130
#define BROWSING_HEIGHT 420

static struct gui_window *window;
static char command[GUI_PATH_MAX], status[GUI_PATH_MAX + 64];
static char history[MAX_HISTORY][GUI_PATH_MAX];
static int history_count, history_at;
static struct gui_file_picker picker;
static bool done, focus_command = true;

static void load_history(void) {
    FILE *f = fopen(HISTORY, "r");

    while (f && history_count < MAX_HISTORY && fgets(history[history_count], GUI_PATH_MAX, f)) {
        history[history_count][strcspn(history[history_count], "\n")] = '\0';
        history_count += history[history_count][0] != '\0';
    }
    if (f)
        fclose(f);
    snprintf(command, sizeof(command), "%s", history_count ? history[0] : "");
}

// The command goes first, and any older copy of it goes.
static void save_history(void) {
    FILE *f = fopen(HISTORY, "w");

    if (!f)
        return;
    fprintf(f, "%s\n", command);
    for (int i = 0, kept = 1; i < history_count && kept < MAX_HISTORY; i++) {
        if (strcmp(history[i], command)) {
            fprintf(f, "%s\n", history[i]);
            kept++;
        }
    }
    fclose(f);
}

// Where a program is, from a path or by looking in PATH.
static bool find_program(const char *name, char out[GUI_PATH_MAX]) {
    const char *path = getenv("PATH");

    if (strchr(name, '/')) {
        snprintf(out, GUI_PATH_MAX, "%s", name);
        return access(out, X_OK) == 0;
    }
    for (const char *p = path ? path : "/usr/bin"; *p;) {
        size_t n = strcspn(p, ":");

        snprintf(out, GUI_PATH_MAX, "%.*s/%s", (int)n, p, name);
        if (access(out, X_OK) == 0)
            return true;
        p += n + (p[n] == ':');
    }
    return false;
}

static bool start(const char *const argv[]) {
    pid_t pid = fork();

    if (pid == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    return pid > 0;
}

// A file or folder opens in the app the settings choose for its kind, but for a
// program, which runs, as does anything else, as a command for the shell.
static void run(void) {
    char first[GUI_PATH_MAX], program[GUI_PATH_MAX];
    struct stat st;
    const char *at = command + strspn(command, " ");
    bool started;

    snprintf(first, sizeof(first), "%.*s", (int)strcspn(at, " "), at);
    if (!first[0]) {
        snprintf(status, sizeof(status), "Type something to open first.");
        return;
    }
    if (!strcmp(first, at) && stat(first, &st) == 0 && (S_ISDIR(st.st_mode) || !gui_is_program(first))) {
        char with[GUI_PATH_MAX];

        started = gui_app_path(gui_app_for(first), with) && start((const char *[]){with, first, NULL});
    } else if (find_program(first, program)) {
        started = start((const char *[]){"/bin/sh", "-c", at, NULL});
    } else {
        snprintf(status, sizeof(status), "Can't find '%s'. Check the name and try again.", first);
        printf("run: can't find %s\n", first);
        return;
    }
    if (!started) {
        snprintf(status, sizeof(status), "Couldn't start '%s'.", first);
        return;
    }
    printf("run: running %s\n", at);
    save_history();
    done = true;
}

// Up and Down go through the commands run before.
static void recall(struct gui_ui *ui) {
    int was = history_at;

    if (!history_count || !gui_focused(ui, COMMAND))
        return;
    if (gui_key(ui, KEY_UP) && history_at < history_count - 1)
        history_at++;
    if (gui_key(ui, KEY_DOWN) && history_at > 0)
        history_at--;
    if (history_at != was) {
        snprintf(command, sizeof(command), "%s", history[history_at]);
        focus_command = true;
    }
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    if (picker.open) {
        int picked = gui_file_picker(ui, &picker);

        if (picked == GUI_PICKED)
            snprintf(command, sizeof(command), "%s", picker.path);
        if (picked) {
            gui_resize(window, WIDTH, HEIGHT);
            focus_command = true;
            ui->again = true;
        }
        gui_ui_end(ui);
        return;
    }
    if (focus_command)
        gui_set_focus(ui, COMMAND, command);
    focus_command = false;
    gui_label(ui, "Type the name of a program, folder or file to open it.");
    gui_label(ui, "Open:");
    gui_same_line(ui);
    gui_textbox(ui, COMMAND, command, sizeof(command));
    recall(ui);
    gui_label(ui, "%s", status);
    if (gui_button(ui, "OK") || (gui_focused(ui, COMMAND) && gui_key(ui, KEY_ENTER)))
        run();
    gui_same_line(ui);
    if (gui_button(ui, "Cancel") || gui_key(ui, KEY_ESCAPE))
        done = true;
    gui_same_line(ui);
    if (gui_button(ui, "Browse...")) {
        gui_resize(window, WIDTH, BROWSING_HEIGHT);
        gui_picker_open(&picker, "/usr/bin", NULL, false);
        ui->again = true;
    }
    gui_ui_end(ui);
    fflush(stdout);
}

// A small window to type a program, folder or file into and open it.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: run\n"
                            "Opens a window to type a program, folder or file into, and opens it.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "run: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;

    window = gui_create_window("Run", WIDTH, HEIGHT);
    if (!window) {
        dprintf(STDERR_FILENO, "run: couldn't make a window\n");
        return 1;
    }
    load_history();
    gui_ui_init(&ui, window);
    frame(&ui);
    printf("run: ready\n");
    fflush(stdout);
    while (!done && gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
