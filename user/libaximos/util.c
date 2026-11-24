#include <aximos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

void help_option(int argc, char **argv, const char *text) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fputs(text, stdout);
        exit(0);
    }
}

void print_size(uint64_t bytes, int width) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    uint64_t tenths = 0;
    int unit = 0;

    while (bytes >= 1024 && unit < 4) {
        tenths = bytes % 1024 * 10 / 1024;
        bytes /= 1024;
        unit++;
    }

    // Below these widths there's nothing to pad, and a negative one would pad.
    if (unit)
        printf("%*lu.%lu %s", width > 6 ? width - 6 : 0, bytes, tenths, units[unit]);
    else
        printf("%*lu %-3s", width > 4 ? width - 4 : 0, bytes, units[unit]);
}

int termsize(int *rows, int *cols) {
    struct winsize w;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) < 0 || !w.ws_row || !w.ws_col) // a serial port doesn't know
        return -1;
    *rows = w.ws_row, *cols = w.ws_col;
    return 0;
}

bool ask(const char *prompt, char *out, int size, bool secret) {
    struct termios was, quiet;
    bool hidden = secret && tcgetattr(STDIN_FILENO, &was) == 0, got;

    fputs(prompt, stdout);
    fflush(stdout);
    if (hidden) {
        quiet = was;
        quiet.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    got = fgets(out, size, stdin) != NULL;
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSANOW, &was);
        fputs("\n", stdout);
    }
    out[strcspn(out, "\n")] = '\0';
    return got;
}

bool ask_new_password(char *out, int size) {
    char again[256];

    for (;;) {
        if (!ask("New password: ", out, size, true) || !ask("Again: ", again, sizeof(again), true))
            return false;
        if (out[0] && !strcmp(out, again))
            break;
        puts(out[0] ? "They're different. Try again." : "It can't be empty.");
    }
    memset(again, 0, sizeof(again));
    return true;
}
