#include <aximos.h>
#include <gui.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// The log-in screen, which wm starts. In greetd's greeter, it logs in through greetd,
// which then starts the desktop as whoever logged in. Locking, it runs as whoever's
// logged in, and PAM checks their password.
enum page { LOG_IN, LOCKED };
static const char *const page_names[] = {"log in", "locked"};
static const int sizes[][2] = {{360, 230}, {360, 200}};

// What greetd starts once someone's logged in: wm, saying what it does to the journal.
#define SESSION "[\"systemd-cat\",\"-t\",\"wm\",\"wm\"]"
#define LOCK_SERVICE "aximos-lock"

static enum page page;
static struct gui_window *window;
static char name[64], password[128], message[96];

// greetd's requests and answers: JSON, each after its length in 4 bytes.

static int greetd = -1;

// Puts a string in JSON's quotes, escaping what has to be.
static void quote(char *out, size_t size, const char *s) {
    size_t n = 0;

    out[n++] = '"';
    for (; *s && n + 8 < size; s++) {
        if (*s == '"' || *s == '\\')
            out[n++] = '\\', out[n++] = *s;
        else if ((unsigned char)*s < 0x20)
            n += snprintf(out + n, size - n, "\\u%04x", *s);
        else
            out[n++] = *s;
    }
    out[n++] = '"';
    out[n] = '\0';
}

// Sends a request and reads the answer, which is NULL if greetd's gone.
static char *ask_greetd(const char *request, char *answer, size_t size) {
    uint32_t length = strlen(request);

    if (greetd < 0 || write(greetd, &length, 4) != 4 || write(greetd, request, length) != (ssize_t)length
        || read(greetd, &length, 4) != 4 || length >= size)
        return NULL;
    for (size_t got = 0; got < length;) {
        ssize_t n = read(greetd, answer + got, length - got);

        if (n <= 0)
            return NULL;
        got += n;
    }
    answer[length] = '\0';
    return answer;
}

static bool is(const char *answer, const char *type) {
    char want[64];

    snprintf(want, sizeof(want), "\"type\":\"%s\"", type);
    return answer && strstr(answer, want);
}

// Logs in through greetd: a session for the name, which asks for the password, then
// the desktop started as them.
static bool greetd_log_in(void) {
    char request[512], quoted[300], answer[4096];
    char *got;

    quote(quoted, sizeof(quoted), name);
    snprintf(request, sizeof(request), "{\"type\":\"create_session\",\"username\":%s}", quoted);
    got = ask_greetd(request, answer, sizeof(answer));
    while (is(got, "auth_message")) { // its password, or anything else it asks, which gets the same
        quote(quoted, sizeof(quoted), password);
        snprintf(request, sizeof(request), "{\"type\":\"post_auth_message_response\",\"response\":%s}", quoted);
        got = ask_greetd(request, answer, sizeof(answer));
    }
    if (is(got, "success")) {
        got = ask_greetd("{\"type\":\"start_session\",\"cmd\":" SESSION ",\"env\":[]}", answer, sizeof(answer));
        if (is(got, "success"))
            return true;
    }
    ask_greetd("{\"type\":\"cancel_session\"}", answer, sizeof(answer));
    return false;
}

static bool connect_greetd(void) {
    const char *path = getenv("GREETD_SOCK");
    struct sockaddr_un address = {.sun_family = AF_UNIX};

    if (!path)
        return false;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    greetd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    return greetd >= 0 && connect(greetd, (struct sockaddr *)&address, sizeof(address)) == 0;
}

// PAM asks for the password, which is all this knows to answer.
static int answer_pam(int count, const struct pam_message **messages, struct pam_response **responses, void *data) {
    struct pam_response *r = calloc(count, sizeof(*r));

    (void)data;
    if (!r)
        return PAM_BUF_ERR;
    for (int i = 0; i < count; i++) {
        if (messages[i]->msg_style == PAM_PROMPT_ECHO_OFF || messages[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup(password);
    }
    *responses = r;
    return PAM_SUCCESS;
}

// Whether the password is whoever's logged in.
static bool check_own(void) {
    struct pam_conv conversation = {answer_pam, NULL};
    pam_handle_t *pam;
    int result = pam_start(LOCK_SERVICE, name, &conversation, &pam);

    if (result == PAM_SUCCESS)
        result = pam_authenticate(pam, 0);
    if (result == PAM_SUCCESS)
        result = pam_acct_mgmt(pam, 0);
    pam_end(pam, result);
    return result == PAM_SUCCESS;
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("login: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void go_to(struct gui_ui *ui, enum page to) {
    page = to;
    message[0] = '\0';
    password[0] = '\0';
    gui_resize(window, sizes[page][0], sizes[page][1]);
    gui_set_focus(ui, page == LOCKED ? "##password" : "##name", NULL);
    ui->again = true;
    printf("login: %s, %dx%d\n", page_names[page], sizes[page][0], sizes[page][1]);
}

// Returns whether the password, and on the log-in page the name, were right.
static bool log_in(struct gui_ui *ui) {
    bool submit = gui_key(ui, KEY_ENTER);

    gui_heading(ui, page == LOCKED ? "Locked" : "Log in");
    if (page == LOCKED) {
        gui_label(ui, "%s is logged in.", name);
    } else {
        gui_label(ui, "Username");
        gui_textbox(ui, "##name", name, sizeof(name));
        report(ui, 0, "name");
    }
    gui_label(ui, "Password");
    gui_password(ui, "##password", password, sizeof(password));
    report(ui, 1, "password");
    if (message[0])
        gui_label(ui, "%s", message);
    submit |= gui_button(ui, page == LOCKED ? "Unlock" : "Log in");
    report(ui, 4, "submit");
    if (!submit)
        return false;

    bool right = page == LOCKED ? check_own() : greetd_log_in();

    memset(password, 0, sizeof(password));
    if (!right) {
        snprintf(message, sizeof(message), page == LOCKED ? "That password isn't right." : "Wrong username or password.");
        printf("login: wrong\n");
        gui_set_focus(ui, "##password", NULL);
        return false;
    }
    printf("login: logged in %s\n", name);
    return true;
}

static bool frame(struct gui_ui *ui) {
    bool done;

    gui_ui_begin(ui);
    done = log_in(ui);
    gui_ui_end(ui);
    fflush(stdout);
    return done;
}

// What wm shows until someone logs in: started by wm, and nothing else can unlock it.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: login [--lock]\n"
                            "The log-in screen, which wm starts in greetd's greeter, or to lock the desktop.\n");
    bool locking = argc > 1 && !strcmp(argv[1], "--lock");
    struct passwd *me = getpwuid(getuid());
    struct gui_message e;
    struct gui_ui ui;
    bool done = false;

    if (!getenv("WAYLAND_SOCKET") || !gui_connect() || (!locking && !connect_greetd())) {
        dprintf(STDERR_FILENO, "login: only wm starts this, in greetd's greeter or to lock\n");
        return 1;
    }
    if (locking && me)
        snprintf(name, sizeof(name), "%s", me->pw_name);
    page = locking ? LOCKED : LOG_IN;
    if (!(window = gui_create_window("Log in", sizes[page][0], sizes[page][1]))) {
        dprintf(STDERR_FILENO, "login: couldn't make its window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    go_to(&ui, page);
    frame(&ui);
    while (!done && gui_next_event(&e)) {
        if (gui_ui_event(&ui, &e))
            done = frame(&ui);
    }
    if (done)
        gui_unlock();
    return !done;
}
