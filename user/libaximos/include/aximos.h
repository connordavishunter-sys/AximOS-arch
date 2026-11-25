#pragma once

#include <stddef.h>
#include <stdint.h>

// What AximOS's programs share beyond glibc, in libaximos.so.

// If the first argument is -h or --help, prints the text and exits.
void help_option(int argc, char **argv, const char *text);

// Prints a size with its unit, like "1.5 MiB", rounded down and right-aligned so
// that units line up in a column. A width of 10 fits every value below 10000 TiB.
void print_size(uint64_t bytes, int width);

// The size of the terminal on standard output, or -1, leaving them as they were, if
// it isn't one or doesn't know, as a serial port doesn't.
int termsize(int *rows, int *cols);

// A process, as Linux's /proc tells it. Ticks are processor time, at sysconf's
// _SC_CLK_TCK a second, and memory is what's in RAM.
struct process {
    int pid, ppid, threads;
    char state; // R running, S sleeping, D waiting on a device, Z exited, I idle
    long ticks;
    uint64_t memory;
    char command[256]; // its arguments, or the kernel thread's name in brackets
};

bool process_read(int pid, struct process *out);

// How long each processor has been idle, and the time since boot, in the same ticks.
#define MAX_CPUS 256
struct cpu_times {
    int count;
    long ticks, idle[MAX_CPUS];
};

bool cpu_times(struct cpu_times *out);

// In bytes. Available is what programs could have without anything being swapped.
struct memory {
    uint64_t total, free, available, cached;
};

bool memory_read(struct memory *out);

// Accounts, in /etc/passwd, /etc/group and /etc/shadow, with passwords hashed by
// yescrypt. People's accounts are numbered from FIRST_UID, and those in the admin
// group may run commands as root with sudo.
#define FIRST_UID 1000
#define ADMIN_GROUP "admin"

// A name is letters, digits, dots, dashes and underscores, starting with a letter.
bool account_name_ok(const char *name);

// Whether anyone has an account yet, besides root.
bool account_any(void);

// Whether the password is the account's. It takes as long whether or not there's
// such an account, so trying names can't find which there are.
bool account_check(const char *name, const char *password);

// Makes an account with its home folder, in the admin group if admin.
bool account_create(const char *name, const char *password, bool admin);

bool account_set_password(const char *name, const char *password);

bool account_is_admin(const char *name);

// Becomes the account, with its groups, home folder and environment, as logging
// in does. Only root can.
bool account_become(const char *name);

// Says who's logged in on a console, such as tty1, in /run/seat, for audiod to know
// whose sound to play: the account, or with NULL, no one. Only root can.
void account_seat(const char *console, const char *name);

// Asks a question at the terminal, reading the answer without its newline, and
// without showing it if it's secret. False at the end of the input.
bool ask(const char *prompt, char *out, int size, bool secret);

// Asks for a new password twice, until both match and it isn't empty.
bool ask_new_password(char *out, int size);
