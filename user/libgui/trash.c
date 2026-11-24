#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Each thing in GUI_TRASH has a random name, of 16 base64 characters, and a line
// in GUI_TRASH_DB with that name, a tab, and the path it had.

#define NAME_LENGTH 16
#define MAX_LINE (NAME_LENGTH + 2 + GUI_PATH_MAX)

static bool copy_file(const char *from, const char *to, mode_t mode) {
    char buffer[16384];
    int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
    ssize_t n = 0;

    while (out >= 0 && (n = read(in, buffer, sizeof(buffer))) > 0 && write(out, buffer, n) == n)
        ;
    if (in >= 0)
        close(in);
    return out >= 0 && close(out) == 0 && n == 0;
}

bool gui_copy_tree(const char *from, const char *to) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    bool ok = true;

    if (stat(from, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return copy_file(from, to, st.st_mode);
    if (mkdir(to, st.st_mode & 0777) < 0 || !(dir = opendir(from)))
        return false;
    while (ok && (d = readdir(dir))) {
        char a[GUI_PATH_MAX], b[GUI_PATH_MAX];

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        snprintf(a, sizeof(a), "%s/%s", from, d->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, d->d_name);
        ok = gui_copy_tree(a, b);
    }
    closedir(dir);
    return ok;
}

bool gui_delete_tree(const char *path) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    bool ok = true;

    if (lstat(path, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    if (!(dir = opendir(path)))
        return false;
    while (ok && (d = readdir(dir))) {
        char inside[GUI_PATH_MAX];

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        snprintf(inside, sizeof(inside), "%s/%s", path, d->d_name);
        ok = gui_delete_tree(inside);
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

// Renames, or copies then deletes between file systems.
static bool move(const char *from, const char *to) {
    return rename(from, to) == 0 || (errno == EXDEV && gui_copy_tree(from, to) && gui_delete_tree(from));
}

static void random_name(char out[NAME_LENGTH + 1]) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint8_t bytes[NAME_LENGTH];

    getentropy(bytes, sizeof(bytes));
    for (int i = 0; i < NAME_LENGTH; i++)
        out[i] = digits[bytes[i] & 63];
    out[NAME_LENGTH] = '\0';
}

// Every line but the one for name, if it's given, written again.
static bool forget(const char *name) {
    char db[GUI_PATH_MAX], temporary[GUI_PATH_MAX + 8];

    snprintf(db, sizeof(db), "%s", GUI_TRASH_DB);
    snprintf(temporary, sizeof(temporary), "%s.new", db);

    FILE *in = fopen(db, "r"), *out = fopen(temporary, "w");
    char line[MAX_LINE];
    size_t length = strlen(name);

    while (in && out && fgets(line, sizeof(line), in)) {
        if (strncmp(line, name, length) || line[length] != '\t')
            fputs(line, out);
    }
    if (in)
        fclose(in);
    return out && fclose(out) == 0 && rename(temporary, db) == 0;
}

bool gui_trash(const char *path) {
    char name[NAME_LENGTH + 1], to[GUI_PATH_MAX], full[GUI_PATH_MAX];
    FILE *f;

    if (path[0] != '/' && getcwd(full, sizeof(full)))
        snprintf(full + strlen(full), sizeof(full) - strlen(full), "/%s", path);
    else
        snprintf(full, sizeof(full), "%s", path);
    if (strchr(full, '\n') || strchr(full, '\t')) {
        errno = EINVAL;
        return false;
    }
    mkdir(GUI_TRASH, 0755);
    do {
        random_name(name);
        snprintf(to, sizeof(to), "%s/%s", GUI_TRASH, name);
    } while (access(to, F_OK) == 0);
    if (!move(full, to))
        return false;
    if (!(f = fopen(GUI_TRASH_DB, "a")))
        return false;
    fprintf(f, "%s\t%s\n", name, full);
    return fclose(f) == 0;
}

bool gui_trash_origin(const char *name, char path[GUI_PATH_MAX]) {
    FILE *f = fopen(GUI_TRASH_DB, "r");
    char line[MAX_LINE];
    size_t length = strlen(name);
    bool found = false;

    while (f && !found && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, name, length) && line[length] == '\t') {
            line[strcspn(line, "\n")] = '\0';
            snprintf(path, GUI_PATH_MAX, "%s", line + length + 1);
            found = true;
        }
    }
    if (f)
        fclose(f);
    return found;
}

// Makes the folders a path is in, that aren't there now.
static void make_parents(const char *path) {
    char part[GUI_PATH_MAX];

    snprintf(part, sizeof(part), "%s", path);
    for (char *slash = strchr(part + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(part, 0755);
        *slash = '/';
    }
}

// Back where it was, or with " (restored)" if something else is there now.
bool gui_trash_restore(const char *name, char path[GUI_PATH_MAX]) {
    char from[GUI_PATH_MAX], origin[GUI_PATH_MAX];

    if (!gui_trash_origin(name, origin))
        return false;
    snprintf(from, sizeof(from), "%s/%s", GUI_TRASH, name);
    snprintf(path, GUI_PATH_MAX, "%s", origin);
    for (int i = 1; access(path, F_OK) == 0; i++)
        snprintf(path, GUI_PATH_MAX, i == 1 ? "%s (restored)" : "%s (restored %d)", origin, i);
    make_parents(path);
    return move(from, path) && forget(name);
}

bool gui_trash_delete(const char *name) {
    char path[GUI_PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", GUI_TRASH, name);
    return gui_delete_tree(path) && forget(name);
}

bool gui_trash_empty(void) {
    DIR *dir = opendir(GUI_TRASH);
    struct dirent *d;
    bool ok = true;

    while (dir && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];

        if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
            snprintf(path, sizeof(path), "%s/%s", GUI_TRASH, d->d_name);
            ok &= gui_delete_tree(path);
        }
    }
    if (dir)
        closedir(dir);
    return ok && (unlink(GUI_TRASH_DB) == 0 || errno == ENOENT);
}
