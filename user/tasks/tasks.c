#include <aximos.h>
#include <dirent.h>
#include <gui.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROCESSES 256
#define ROW_MAX 96
#define UPDATE_MS 1500
#define GRAPH_HEIGHT 60
#define SAMPLES 120 // of the CPU and memory, a minute's worth at most shown

// What's running, how busy each is, and how much memory each has, updated every
// UPDATE_MS, with buttons to end one.
struct task {
    int pid, tenths; // of a percent of the time since the last update
    long ticks, memory_kb;
    char name[48];
};

static struct task processes[MAX_PROCESSES], last[MAX_PROCESSES];
static int count, last_count, selected = -1, selected_pid = -1, sort;
static bool reversed, confirming;
static int cpu_samples[SAMPLES], memory_samples[SAMPLES], samples;
static struct cpu_times now;
static int cpu_percent;
static long memory_total, memory_used;
static char rows[MAX_PROCESSES][ROW_MAX], status[96];
static const char *row_pointers[MAX_PROCESSES];
static const char *const sorts[] = {"CPU", "Memory", "Name", "PID"};

// The columns, as wide as the rows' text, which sort by them when clicked.
static const char *const headers[] = {"PID", "Name", "CPU", "Memory"};
static const int header_sorts[] = {3, 2, 0, 1}, header_widths[] = {56, 224, 64, 88};

static long last_ticks(int pid) {
    for (int i = 0; i < last_count; i++) {
        if (last[i].pid == pid)
            return last[i].ticks;
    }
    return -1;
}

static int compare(const void *left, const void *right) {
    const struct task *a = left, *b = right;
    int by = sort == 0   ? b->tenths - a->tenths
           : sort == 1 ? (b->memory_kb > a->memory_kb) - (b->memory_kb < a->memory_kb)
           : sort == 2 ? strcmp(a->name, b->name)
                       : 0;

    by = by ? by : a->pid - b->pid;
    return reversed ? -by : by;
}

static void read_system(long *elapsed) {
    struct cpu_times before = now;
    struct memory m;
    long idle = 0;

    cpu_times(&now);
    for (int i = 0; i < now.count; i++)
        idle += now.idle[i] - before.idle[i];
    *elapsed = before.ticks ? now.ticks - before.ticks : 0;
    cpu_percent = *elapsed > 0 && now.count ? 100 - (int)(idle * 100 / (*elapsed * now.count)) : 0;
    cpu_percent = cpu_percent < 0 ? 0 : cpu_percent > 100 ? 100 : cpu_percent;
    if (memory_read(&m)) {
        memory_total = m.total / 1024;
        memory_used = (m.total - m.available) / 1024;
    }
    if (samples == SAMPLES) {
        memmove(cpu_samples, cpu_samples + 1, (SAMPLES - 1) * sizeof(int));
        memmove(memory_samples, memory_samples + 1, (SAMPLES - 1) * sizeof(int));
        samples--;
    }
    cpu_samples[samples] = cpu_percent;
    memory_samples[samples++] = memory_total ? (int)(memory_used * 100 / memory_total) : 0;
}

static void format_size(long kb, char out[16]) {
    if (kb >= 10 * 1024)
        snprintf(out, 16, "%ld MB", kb / 1024);
    else
        snprintf(out, 16, "%ld KB", kb);
}

static void update(void) {
    long elapsed = 0;
    DIR *proc = opendir("/proc");
    struct dirent *e;

    read_system(&elapsed);
    memcpy(last, processes, sizeof(last));
    last_count = count;
    count = 0;
    while (proc && (e = readdir(proc)) && count < MAX_PROCESSES) {
        struct task *p = &processes[count];
        struct process found;

        if (!atoi(e->d_name) || !process_read(atoi(e->d_name), &found))
            continue;
        p->pid = found.pid;
        p->ticks = found.ticks;
        p->memory_kb = found.memory / 1024;

        long before = last_ticks(p->pid);

        p->tenths = elapsed > 0 && before >= 0 ? (int)((p->ticks - before) * 1000 / elapsed) : 0;
        snprintf(p->name, sizeof(p->name), "%s", found.command);
        count++;
    }
    if (proc)
        closedir(proc);
    qsort(processes, count, sizeof(*processes), compare);

    // The selection stays on the same process, wherever it's sorted to.
    selected = -1;
    for (int i = 0; i < count; i++) {
        struct task *p = &processes[i];
        char memory[16];

        format_size(p->memory_kb, memory);
        snprintf(rows[i], ROW_MAX, "%5d  %-28.28s %4d.%d%%  %8s", p->pid, p->name, p->tenths / 10, p->tenths % 10,
                 memory);
        row_pointers[i] = rows[i];
        if (p->pid == selected_pid)
            selected = i;
    }
    if (selected < 0)
        selected_pid = -1;
}

static void end(bool force) {
    if (selected < 0)
        return;

    struct task *p = &processes[selected];

    if (p->pid == 1)
        snprintf(status, sizeof(status), "init can't be ended.");
    else if (kill(p->pid, force ? SIGKILL : SIGTERM) < 0)
        snprintf(status, sizeof(status), "Couldn't end %s.", p->name);
    else
        snprintf(status, sizeof(status), "Ended %s.", p->name);
    printf("tasks: %s\n", status);
    update();
}

static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("tasks: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

// The CPU, in the accent colour, and memory, in green, over the last SAMPLES updates.
static void graph(struct gui_ui *ui) {
    struct gui_settings settings;
    struct gui_rect r = gui_space(ui, 0, GRAPH_HEIGHT);
    struct gui_canvas *c = &ui->window->canvas;
    int step = r.width / SAMPLES > 0 ? r.width / SAMPLES : 1;

    gui_settings_load(&settings);
    gui_fill(c, r.x, r.y, r.width, r.height, settings.light ? 0xFFFFFF : 0x181B21);
    for (int i = 1; i < 4; i++)
        gui_fill(c, r.x, r.y + r.height * i / 4, r.width, 1, settings.light ? 0xE2E5EB : 0x2B303B);
    for (int i = 0; i < samples; i++) {
        int x = r.x + r.width - (samples - i) * step;
        int cpu = cpu_samples[i] * (r.height - 2) / 100, memory = memory_samples[i] * (r.height - 2) / 100;

        if (x < r.x)
            continue;
        gui_fill(c, x, r.y + r.height - 1 - cpu, step, cpu + 1, 0x3D63DD);
        gui_fill(c, x, r.y + r.height - 2 - memory, step, 2, 0x27AE60);
    }
}

static void sort_by(int by) {
    reversed = sort == by && !reversed;
    sort = by;
    printf("tasks: sort by %s%s\n", sorts[sort], reversed ? ", reversed" : "");
    update();
}

static void frame(struct gui_ui *ui) {
    static const char *const buttons[] = {"End task", "Cancel"};
    char total[16], used[16], question[128];

    gui_ui_begin(ui);
    if (confirming && selected >= 0) {
        snprintf(question, sizeof(question), "End %s, process %d?\nWhat it hasn't saved is lost.", processes[selected].name,
                 processes[selected].pid);

        int button = gui_dialog(ui, question, buttons, 2);

        if (button == 1)
            end(false);
        confirming = !button;
    }
    format_size(memory_used, used);
    format_size(memory_total, total);
    gui_label(ui, "CPU %d%%    Memory %s of %s    %d processes", cpu_percent, used, total, count);
    graph(ui);
    if ((gui_button(ui, "End task") || gui_key(ui, KEY_DELETE)) && selected >= 0) {
        confirming = true;
        printf("tasks: asking to end %d\n", processes[selected].pid);
        ui->again = true;
    }
    report(ui, 0, "end");
    gui_same_line(ui);
    if (gui_button(ui, "Force end"))
        end(true);
    report(ui, 1, "force");
    if (status[0]) {
        gui_same_line(ui);
        gui_label(ui, "%s", status);
    }

    // The headers, over the list's columns.
    for (int i = 0; i < 4; i++) {
        char label[32];

        snprintf(label, sizeof(label), "%s%s", headers[i],
                 sort == header_sorts[i] ? (reversed ? " ^" : " v") : "");
        if (i)
            gui_same_line(ui);
        if (gui_button_sized(ui, label, header_widths[i], 0))
            sort_by(header_sorts[i]);
        report(ui, 4 + i, headers[i]);
    }
    if (gui_list(ui, "##processes", row_pointers, count, &selected, 0) && selected >= 0 && !confirming) {
        selected_pid = processes[selected].pid;
        printf("tasks: selected %d %s\n", selected_pid, processes[selected].name);
    }
    report(ui, 2, "list");
    gui_ui_end(ui);
    fflush(stdout);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: tasks\n"
                            "Shows what's running, how busy it is and the memory it has, and ends what's chosen.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "tasks: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Task Manager", 560, 440);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "tasks: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    update();
    frame(&ui);
    gui_timer_start(UPDATE_MS, true);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_TIMER && !confirming)
            update();
        if (gui_ui_event(&ui, &e) || e.type == GUI_TIMER)
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
