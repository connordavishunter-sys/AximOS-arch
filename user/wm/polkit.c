#include "polkit.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <unistd.h>

// polkit's agent interface, on the system bus. BeginAuthentication's answered once
// the password's right, or the request's cancelled. The password's checked by
// polkit's helper, through its socket: the user's name and the request's cookie,
// then the password for each prompt, until it says SUCCESS or FAILURE.

#define AGENT_PATH "/org/aximos/PolkitAgent"
#define HELPER "/run/polkit/agent-helper.socket"
#define CANCELLED "org.freedesktop.PolicyKit1.Error.Cancelled"

static sd_bus *bus;
static sd_bus_message *call; // the BeginAuthentication being answered
static char cookie[256];
static int helper = -1;
static char heard[1024], password[256];
static size_t heard_length;
static bool answered_prompt;
static struct polkit_request now;

static void finish(const char *error) {
    if (call) {
        if (error)
            sd_bus_reply_method_errorf(call, error, "%s", "Cancelled");
        else
            sd_bus_reply_method_return(call, "");
        sd_bus_message_unref(call);
        call = NULL;
    }
    if (helper >= 0)
        close(helper);
    helper = -1;
    explicit_bzero(password, sizeof(password));
    now = (struct polkit_request){0};
}

// Whose password: wm's own account's, if it's one polkit will take, as it is for an
// admin, or else the first account it names, such as root.
static void choose_user(sd_bus_message *m) {
    uid_t first = (uid_t)-1;
    bool own = false;

    if (sd_bus_message_enter_container(m, 'a', "(sa{sv})") < 0)
        return;
    while (sd_bus_message_enter_container(m, 'r', "sa{sv}") > 0) {
        const char *kind;

        sd_bus_message_read(m, "s", &kind);
        sd_bus_message_enter_container(m, 'a', "{sv}");
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char *key;
            uint32_t uid;

            sd_bus_message_read(m, "s", &key);
            if (!strcmp(kind, "unix-user") && !strcmp(key, "uid") && sd_bus_message_read(m, "v", "u", &uid) >= 0) {
                own |= uid == getuid();
                first = first == (uid_t)-1 ? uid : first;
            } else {
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);

    struct passwd *pw = getpwuid(own ? getuid() : first);

    snprintf(now.user, sizeof(now.user), "%s", pw ? pw->pw_name : "root");
}

static int begin(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *action, *message, *icon, *given;

    (void)data, (void)e;
    if (call) // one at a time
        return sd_bus_reply_method_errorf(m, CANCELLED, "Another authentication is in progress");
    if (sd_bus_message_read(m, "sss", &action, &message, &icon) < 0 || sd_bus_message_skip(m, "a{ss}") < 0
        || sd_bus_message_read(m, "s", &given) < 0)
        return sd_bus_reply_method_errorf(m, CANCELLED, "Couldn't read the request");
    now = (struct polkit_request){.asking = true};
    snprintf(now.message, sizeof(now.message), "%s", message);
    snprintf(cookie, sizeof(cookie), "%s", given);
    choose_user(m);
    call = sd_bus_message_ref(m);
    printf("wm: asked for %s's password, for %s\n", now.user, action);
    return 1;
}

static int cancel(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *which;

    (void)data, (void)e;
    if (sd_bus_message_read(m, "s", &which) >= 0 && call && !strcmp(which, cookie)) {
        printf("wm: polkit cancelled asking for a password\n");
        finish(CANCELLED);
    }
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("BeginAuthentication", "sssa{ss}sa(sa{sv})", "", begin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CancelAuthentication", "s", "", cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

bool polkit_open(void) {
    char *session = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r;

    if (sd_pid_get_session(0, &session) < 0 || sd_bus_open_system(&bus) < 0
        || sd_bus_add_object_vtable(bus, NULL, AGENT_PATH, "org.freedesktop.PolicyKit1.AuthenticationAgent", agent, NULL) < 0) {
        free(session);
        return false;
    }
    r = sd_bus_call_method(bus, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
                           "org.freedesktop.PolicyKit1.Authority", "RegisterAuthenticationAgent", &e, NULL, "(sa{sv})ss",
                           "unix-session", 1, "session-id", "s", session, "C.UTF-8", AGENT_PATH);
    if (r < 0)
        printf("wm: polkit wouldn't take wm as its agent: %s\n", e.message ? e.message : strerror(-r));
    else
        printf("wm: polkit's agent for session %s\n", session);
    sd_bus_error_free(&e);
    free(session);
    return r >= 0;
}

void polkit_poll(struct pollfd fds[2], int *timeout) {
    uint64_t usec = UINT64_MAX;

    fds[0] = (struct pollfd){.fd = bus ? sd_bus_get_fd(bus) : -1, .events = bus ? sd_bus_get_events(bus) : 0};
    fds[1] = (struct pollfd){.fd = helper, .events = POLLIN};
    *timeout = -1;
    if (bus && sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
        struct timespec t;
        int64_t left;

        clock_gettime(CLOCK_MONOTONIC, &t);
        left = (int64_t)(usec / 1000) - ((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
        *timeout = left > 0 ? (int)left : 0;
    }
}

// What the helper says, a line at a time.
static bool hear(void) {
    ssize_t n;
    bool changed = false;

    while ((n = read(helper, heard + heard_length, sizeof(heard) - 1 - heard_length)) > 0)
        heard_length += n;
    heard[heard_length] = '\0';
    for (char *end; helper >= 0 && (end = strchr(heard, '\n'));) {
        *end = '\0';
        if (!strncmp(heard, "PAM_PROMPT_ECHO_", 16)) {
            if (!answered_prompt) // the password; anything more it can't answer
                dprintf(helper, "%s\n", password);
            else
                shutdown(helper, SHUT_WR);
            answered_prompt = true;
        } else if (!strcmp(heard, "SUCCESS")) {
            printf("wm: the password was right\n");
            finish(NULL);
            return true;
        } else if (!strcmp(heard, "FAILURE")) {
            printf("wm: the password wasn't right\n");
            close(helper);
            helper = -1;
            explicit_bzero(password, sizeof(password));
            now.checking = false, now.wrong = true;
            return true;
        }
        memmove(heard, end + 1, heard_length - (end + 1 - heard) + 1);
        heard_length -= end + 1 - heard;
        changed = true;
    }
    if (n == 0 && helper >= 0) { // gone without saying
        close(helper);
        helper = -1;
        now.checking = false, now.wrong = true;
        return true;
    }
    return changed;
}

bool polkit_read(void) {
    struct polkit_request was = now;

    while (bus && sd_bus_process(bus, NULL) > 0)
        ;
    if (helper >= 0)
        hear();
    return memcmp(&was, &now, sizeof(now)) != 0;
}

const struct polkit_request *polkit_now(void) {
    return &now;
}

void polkit_answer(const char *given) {
    struct sockaddr_un at = {.sun_family = AF_UNIX, .sun_path = HELPER};

    if (!call || now.checking)
        return;
    snprintf(password, sizeof(password), "%s", given);
    helper = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (helper < 0 || connect(helper, (struct sockaddr *)&at, sizeof(at)) < 0) {
        printf("wm: can't reach polkit's helper: %s\n", strerror(errno));
        if (helper >= 0)
            close(helper);
        helper = -1;
        now.wrong = true;
        return;
    }
    dprintf(helper, "%s\n%s\n", now.user, cookie);
    fcntl(helper, F_SETFL, O_NONBLOCK);
    heard_length = 0;
    answered_prompt = false;
    now.checking = true, now.wrong = false;
}

void polkit_cancel(void) {
    if (call)
        printf("wm: asking for a password cancelled\n");
    finish(CANCELLED);
}
