#include <aximos.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FILE_MAX 4096

static ssize_t read_text(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, out, size - 1);

    if (fd >= 0)
        close(fd);
    out[got > 0 ? got : 0] = '\0';
    return got;
}

// The number after a name in a file of lines like "Name:   value".
static long field(const char *text, const char *name) {
    size_t length = strlen(name);

    for (const char *line = text; line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (!strncmp(line, name, length) && line[length] == ':')
            return atol(line + length + 1);
    }
    return 0;
}

bool process_read(int pid, struct process *out) {
    char path[64], text[FILE_MAX];
    ssize_t got;

    // stat has the name in brackets, which can hold spaces, so the rest is after the last ")".
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_text(path, text, sizeof(text)) <= 0 || !strrchr(text, ')'))
        return false;

    char *name = strchr(text, '('), *rest = strrchr(text, ')');
    long utime = 0, stime = 0;

    *rest = '\0';
    *out = (struct process){.pid = pid};
    snprintf(out->command, sizeof(out->command), "[%s]", name + 1);
    sscanf(rest + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld %*d %*d %*d %*d %d", &out->state, &out->ppid,
           &utime, &stime, &out->threads);
    out->ticks = utime + stime;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    if (read_text(path, text, sizeof(text)) > 0)
        out->memory = (uint64_t)field(text, "VmRSS") * 1024;

    // Its arguments, each ending in a zero byte, unless it's a kernel thread, which has none.
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    if ((got = read_text(path, text, sizeof(out->command))) > 0) {
        for (ssize_t i = 0; i < got - 1; i++)
            text[i] = text[i] ? text[i] : ' ';
        snprintf(out->command, sizeof(out->command), "%s", text);
    }
    return true;
}

bool cpu_times(struct cpu_times *out) {
    char text[FILE_MAX * 4];
    double uptime;

    *out = (struct cpu_times){0};
    if (read_text("/proc/uptime", text, sizeof(text)) <= 0 || sscanf(text, "%lf", &uptime) != 1)
        return false;
    out->ticks = (long)(uptime * sysconf(_SC_CLK_TCK));
    if (read_text("/proc/stat", text, sizeof(text)) <= 0)
        return false;

    // Lines like "cpu3 user nice system idle iowait ...", after the total's "cpu ".
    for (char *line = text; line && out->count < MAX_CPUS; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        long idle, iowait;

        if (!strncmp(line, "cpu", 3) && line[3] != ' ' && sscanf(line, "%*s %*d %*d %*d %ld %ld", &idle, &iowait) == 2)
            out->idle[out->count++] = idle + iowait;
    }
    return true;
}

bool memory_read(struct memory *out) {
    char text[FILE_MAX];

    if (read_text("/proc/meminfo", text, sizeof(text)) <= 0)
        return false;
    *out = (struct memory){(uint64_t)field(text, "MemTotal") * 1024, (uint64_t)field(text, "MemFree") * 1024,
                           (uint64_t)field(text, "MemAvailable") * 1024,
                           (uint64_t)(field(text, "Cached") + field(text, "Buffers")) * 1024};
    return true;
}
