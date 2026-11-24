#include <aximos.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define USAGE "usage: notify [-a label command] title [text]\n"

// Shows a notification from the shell, as programs do.
int main(int argc, char **argv) {
    const char *label = NULL, *command = NULL;

    help_option(argc, argv, USAGE "Shows a notification in the corner of the screen for a few seconds.\n"
                                  "With -a, it has a button labelled label, which runs command.\n");
    if (argc > 3 && !strcmp(argv[1], "-a")) {
        label = argv[2];
        command = argv[3];
        argc -= 3;
        argv += 3;
    }
    if (argc < 2 || argc > 3) {
        dprintf(STDERR_FILENO, USAGE);
        return 2;
    }
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "notify: wm isn't running\n");
        return 1;
    }
    return gui_notify_action(NULL, argv[1], argc > 2 ? argv[2] : "", label, command) ? 0 : 1;
}
