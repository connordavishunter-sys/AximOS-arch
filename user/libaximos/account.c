#include <aximos.h>
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PASSWD "/etc/passwd"
#define GROUP "/etc/group"
#define SHADOW_FILE "/etc/shadow"
#define SKEL "/etc/skel" // what each new home starts with
#define SHELL "/bin/bash"
#define USERS_GID 100
#define LINE_SIZE 1024

bool account_name_ok(const char *name) {
    if (!isalpha((unsigned char)name[0]) || strlen(name) > 31)
        return false;
    for (const char *c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && !strchr("._-", *c))
            return false;
    }
    return true;
}

bool account_any(void) {
    struct passwd *p;
    bool found = false;

    setpwent();
    while (!found && (p = getpwent()))
        found = p->pw_uid >= FIRST_UID && p->pw_uid < 65534;
    endpwent();
    return found;
}

static bool hash(const char *password, char *out, size_t size);

bool account_check(const char *name, const char *password) {
    static char nobody[256]; // a hash of nothing, so a name without an account takes as long
    struct spwd *s = getspnam(name);
    struct crypt_data data = {0};

    if (!nobody[0] && !hash("", nobody, sizeof(nobody)))
        return false;

    bool real = s && s->sp_pwdp[0] == '$';
    const char *stored = real ? s->sp_pwdp : nobody, *tried = crypt_r(password, stored, &data);

    return real && tried && !strcmp(tried, stored);
}

static bool hash(const char *password, char *out, size_t size) {
    struct crypt_data data = {0};
    const char *salt = crypt_gensalt_ra(NULL, 0, NULL, 0), *hashed = salt ? crypt_r(password, salt, &data) : NULL;
    bool ok = hashed && hashed[0] == '$' && strlen(hashed) < size;

    if (ok)
        strcpy(out, hashed);
    free((void *)salt);
    return ok;
}

// Rewrites a file, giving each line to change, which may replace it, and appending
// extra if it's not NULL. Others' changes can't interleave, as lckpwdf locks them out.
static bool rewrite(const char *path, mode_t mode, bool (*change)(char *line, size_t size, const void *with),
                    const void *with, const char *extra) {
    char temporary[64], line[LINE_SIZE];
    FILE *in = fopen(path, "r"), *out;
    bool ok;

    snprintf(temporary, sizeof(temporary), "%s+", path);
    if (!(out = fopen(temporary, "w"))) {
        if (in)
            fclose(in);
        return false;
    }
    fchmod(fileno(out), mode);
    while (in && fgets(line, sizeof(line), in)) {
        if (change)
            change(line, sizeof(line), with);
        fputs(line, out);
    }
    if (extra)
        fputs(extra, out);
    ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
    ok &= fclose(out) == 0;
    if (in)
        fclose(in);
    return ok && rename(temporary, path) == 0;
}

struct new_password {
    const char *name, *hash;
};

static bool replace_password(char *line, size_t size, const void *with) {
    const struct new_password *n = with;
    size_t length = strlen(n->name);
    char *rest;

    if (strncmp(line, n->name, length) || line[length] != ':' || !(rest = strchr(line + length + 1, ':')))
        return false;
    char tail[LINE_SIZE];

    snprintf(tail, sizeof(tail), "%s", rest);
    snprintf(line, size, "%s:%s%s", n->name, n->hash, tail);
    return true;
}

static bool add_to_admin(char *line, size_t size, const void *with) {
    size_t length = strlen(line);

    if (strncmp(line, ADMIN_GROUP ":", strlen(ADMIN_GROUP) + 1))
        return false;
    line[strcspn(line, "\n")] = '\0';
    snprintf(line + strlen(line), size - length, "%s%s\n", line[strlen(line) - 1] == ':' ? "" : ",", (const char *)with);
    return true;
}

bool account_set_password(const char *name, const char *password) {
    char hashed[256];
    struct new_password n = {name, hashed};
    bool ok;

    if (!getpwnam(name) || !hash(password, hashed, sizeof(hashed)) || lckpwdf() < 0)
        return false;
    ok = rewrite(SHADOW_FILE, 0600, replace_password, &n, NULL);
    ulckpwdf();
    return ok;
}

// Copies the folder at from into to, its files and folders owned by uid.
static bool copy_tree(const char *from, const char *to, uid_t uid) {
    DIR *d = opendir(from);
    struct dirent *e;
    bool ok = true;

    if (!d)
        return errno == ENOENT;
    while (ok && (e = readdir(d))) {
        char src[PATH_MAX], dst[PATH_MAX], buf[65536];
        struct stat st;
        ssize_t n;
        int in, out;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(src, sizeof(src), "%s/%s", from, e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
        if (lstat(src, &st) < 0)
            ok = false;
        else if (S_ISDIR(st.st_mode))
            ok = (mkdir(dst, st.st_mode & 07777) == 0 || errno == EEXIST) && lchown(dst, uid, USERS_GID) == 0
                 && copy_tree(src, dst, uid);
        else if (S_ISREG(st.st_mode)) {
            if ((in = open(src, O_RDONLY)) < 0)
                ok = false;
            else if ((out = open(dst, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777)) < 0) {
                ok = errno == EEXIST; // kept, as a home that's there has it already
                close(in);
            } else {
                while ((n = read(in, buf, sizeof(buf))) > 0 && write(out, buf, n) == n)
                    ;
                ok = n == 0 && fchown(out, uid, USERS_GID) == 0;
                close(in);
                ok = close(out) == 0 && ok;
            }
        }
    }
    closedir(d);
    return ok;
}

bool account_create(const char *name, const char *password, bool admin) {
    char hashed[256], home[300], line[LINE_SIZE];
    uid_t uid = FIRST_UID;
    struct passwd *p;
    bool ok;

    if (!account_name_ok(name) || getpwnam(name) || !hash(password, hashed, sizeof(hashed)))
        return false;
    setpwent();
    while ((p = getpwent()))
        uid = p->pw_uid >= uid && p->pw_uid < 65534 ? p->pw_uid + 1 : uid;
    endpwent();
    snprintf(home, sizeof(home), "/home/%s", name);
    if (lckpwdf() < 0)
        return false;
    snprintf(line, sizeof(line), "%s:x:%u:%u::%s:%s\n", name, uid, USERS_GID, home, SHELL);
    ok = rewrite(PASSWD, 0644, NULL, NULL, line);
    snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::\n", name, hashed, (long)(time(NULL) / 86400));
    ok = ok && rewrite(SHADOW_FILE, 0600, NULL, NULL, line);
    ok = ok && (!admin || rewrite(GROUP, 0644, add_to_admin, name, NULL));
    ulckpwdf();
    if (ok && (mkdir(home, 0700) == 0 || errno == EEXIST))
        ok = chown(home, uid, USERS_GID) == 0 && copy_tree(SKEL, home, uid);
    return ok;
}

bool account_is_admin(const char *name) {
    struct group *g = getgrnam(ADMIN_GROUP);

    for (char **member = g ? g->gr_mem : NULL; member && *member; member++) {
        if (!strcmp(*member, name))
            return true;
    }
    return false;
}

bool account_become(const char *name) {
    struct passwd *p = getpwnam(name);

    if (!p || initgroups(p->pw_name, p->pw_gid) < 0 || setgid(p->pw_gid) < 0 || setuid(p->pw_uid) < 0)
        return false;
    setenv("HOME", p->pw_dir, 1);
    setenv("USER", p->pw_name, 1);
    setenv("LOGNAME", p->pw_name, 1);
    setenv("SHELL", p->pw_shell, 1);
    return chdir(p->pw_dir) == 0 || chdir("/") == 0;
}

void account_seat(const char *console, const char *name) {
    struct passwd *p = name ? getpwnam(name) : NULL;
    char path[64];
    FILE *f;

    if (!console || strchr(console, '/'))
        return;
    mkdir("/run/seat", 0755);
    snprintf(path, sizeof(path), "/run/seat/%s", console);
    if (!p) {
        unlink(path);
        return;
    }
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%u\n", (unsigned)p->pw_uid);
        fclose(f);
    }
}
