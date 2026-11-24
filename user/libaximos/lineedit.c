#include <dirent.h>
#include <errno.h>
#include <aximos.h>
#include <lineedit.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define BIN_DIR "/usr/bin"
#define MAX_LINE 1024
#define HISTORY_SIZE 200
#define MAX_MATCHES 512
#define CONTROL(c) ((c) & 0x1F)

#define IS_OPERATOR(c) ((c) == '|' || (c) == '<' || (c) == '>' || (c) == ';' || (c) == '&')
#define STARTS_COMMAND(c) ((c) == '|' || (c) == ';' || (c) == '&')

static char line[MAX_LINE];
static size_t line_len;
static size_t line_pos;

static const char *prompt;
static size_t prompt_len;
static int screen_cols = 80;

// How many rows the cursor is below the row the prompt starts on.
static int cursor_row;

static char *history[HISTORY_SIZE];
static const char *history_file = ".history"; // in the home folder, which keeps it between sessions
static int history_count;

struct match {
    char *name;
    bool is_dir;
};

static struct match matches[MAX_MATCHES];
static int match_count;

static const char *const *extra_commands;

// Redraws the prompt and line, leaving the cursor at line_pos. The console wraps
// as soon as a row is full, so positions follow from simple division.
static void refresh(void) {
    if (cursor_row > 0)
        printf("\x1b[%dA", cursor_row);
    printf("\r%s%s\x1b[J", prompt, line);

    size_t end = prompt_len + line_len;
    size_t target = prompt_len + line_pos;
    int up = end / screen_cols - target / screen_cols;

    if (up > 0)
        printf("\x1b[%dA", up);
    printf("\r");
    if (target % screen_cols > 0)
        printf("\x1b[%dC", (int)(target % screen_cols));

    cursor_row = target / screen_cols;
}

static void set_line(const char *text) {
    line_len = strlen(text);
    memmove(line, text, line_len + 1);
    line_pos = line_len;
}

static void insert_text(const char *text, size_t len) {
    if (line_len + len >= MAX_LINE)
        return;

    memmove(&line[line_pos + len], &line[line_pos], line_len - line_pos + 1);
    memcpy(&line[line_pos], text, len);
    line_pos += len;
    line_len += len;
}

static void delete_range(size_t from, size_t to) {
    memmove(&line[from], &line[to], line_len - to + 1);
    line_len -= to - from;
    line_pos = from;
}

static bool history_add(const char *text) {
    if (text[0] == '\0' || (history_count > 0 && strcmp(history[history_count - 1], text) == 0))
        return false;

    if (history_count == HISTORY_SIZE) {
        free(history[0]);
        memmove(&history[0], &history[1], sizeof(history[0]) * (HISTORY_SIZE - 1));
        history_count--;
    }

    char *copy = malloc(strlen(text) + 1);
    if (copy)
        history[history_count++] = strcpy(copy, text);
    return copy;
}

static FILE *open_history(const char *mode) {
    const char *home = getenv("HOME");
    char path[256];

    if (!home || !*home)
        return NULL;
    snprintf(path, sizeof(path), "%s/%s", strcmp(home, "/") ? home : "", history_file);
    return fopen(path, mode);
}

// Reads what earlier sessions kept, writing it back shorter once it's twice what's kept.
static void load_history(void) {
    char text[MAX_LINE];
    int lines = 0;
    FILE *f = open_history("r");

    if (!f)
        return;
    while (fgets(text, sizeof(text), f)) {
        text[strcspn(text, "\n")] = '\0';
        history_add(text);
        lines++;
    }
    fclose(f);
    if (lines > 2 * HISTORY_SIZE && (f = open_history("w"))) {
        for (int i = 0; i < history_count; i++)
            fprintf(f, "%s\n", history[i]);
        fclose(f);
    }
}

static void save_history(const char *text) {
    FILE *f = open_history("a");

    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

// Where the word before or after the cursor starts or ends, words being split by spaces.
static size_t word_before(void) {
    size_t at = line_pos;

    while (at > 0 && line[at - 1] == ' ')
        at--;
    while (at > 0 && line[at - 1] != ' ')
        at--;
    return at;
}

static size_t word_after(void) {
    size_t at = line_pos;

    while (at < line_len && line[at] == ' ')
        at++;
    while (at < line_len && line[at] != ' ')
        at++;
    return at;
}

static void clear_matches(void) {
    for (int i = 0; i < match_count; i++)
        free(matches[i].name);
    match_count = 0;
}

// Keeps the matches sorted and free of duplicates.
static void add_match(const char *name, bool is_dir) {
    int at = 0;

    while (at < match_count && strcmp(matches[at].name, name) < 0)
        at++;
    if (match_count == MAX_MATCHES || (at < match_count && strcmp(matches[at].name, name) == 0))
        return;

    char *copy = malloc(strlen(name) + 1);
    if (!copy)
        return;

    memmove(&matches[at + 1], &matches[at], sizeof(matches[0]) * (match_count - at));
    matches[at] = (struct match){strcpy(copy, name), is_dir};
    match_count++;
}

static void collect_dir_matches(const char *dir_path, const char *prefix) {
    DIR *dir = opendir(dir_path);
    if (!dir)
        return;

    size_t len = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncasecmp(entry->d_name, prefix, len) == 0)
            add_match(entry->d_name, entry->d_type == DT_DIR);
    }
    closedir(dir);
}

// Finds the word being typed, decoding its quotes and backslashes. Returns where
// it starts in the line and sets whether it is the command word.
static size_t current_word(char *word, bool *is_command) {
    size_t start = line_pos;
    size_t out = 0;
    int index = -1;
    bool in_word = false;
    bool quoted = false;
    bool is_redirect_target = false;

    for (size_t i = 0; i < line_pos; i++) {
        char c = line[i];

        if (!quoted && c == ' ') {
            in_word = false;
            continue;
        }
        if (!quoted && IS_OPERATOR(c)) {
            in_word = false;
            is_redirect_target = !STARTS_COMMAND(c);
            if (STARTS_COMMAND(c))
                index = -1;
            continue;
        }
        if (!in_word) {
            in_word = true;
            start = i;
            out = 0;
            index++;
        }
        if (c == '\\' && i + 1 < line_pos)
            word[out++] = line[++i];
        else if (c == '"')
            quoted = !quoted;
        else
            word[out++] = c;
    }

    if (!in_word) {
        start = line_pos;
        out = 0;
        index++;
    }
    word[out] = '\0';
    *is_command = index == 0 && !is_redirect_target && !strchr(word, '/');
    return start;
}

static size_t common_prefix_len(void) {
    size_t len = strlen(matches[0].name);

    for (int i = 1; i < match_count; i++) {
        size_t same = 0;

        while (same < len && strncasecmp(&matches[0].name[same], &matches[i].name[same], 1) == 0)
            same++;
        len = same;
    }
    return len;
}

static void show_matches(void) {
    size_t width = 0;

    for (int i = 0; i < match_count; i++) {
        size_t len = strlen(matches[i].name) + matches[i].is_dir + 2;
        if (len > width)
            width = len;
    }

    int per_row = screen_cols / width;
    if (per_row < 1)
        per_row = 1;

    size_t saved_pos = line_pos;
    line_pos = line_len;
    refresh();
    printf("\n");

    for (int i = 0; i < match_count; i++) {
        printf("%s%s", matches[i].name, matches[i].is_dir ? "/" : "");
        size_t used = strlen(matches[i].name) + matches[i].is_dir;
        if ((i + 1) % per_row == 0 || i + 1 == match_count)
            printf("\n");
        else
            printf("%*s", (int)(width - used), "");
    }

    line_pos = saved_pos;
    cursor_row = 0;
}

// Writes text with spaces and other special characters escaped.
static size_t escape_word(const char *text, size_t len, char *out) {
    size_t used = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == ' ' || text[i] == '"' || text[i] == '\\' || IS_OPERATOR(text[i]))
            out[used++] = '\\';
        out[used++] = text[i];
    }
    return used;
}

static void complete(bool repeated) {
    static char word[MAX_LINE];
    static char replacement[MAX_LINE * 2];
    bool is_command;
    size_t start = current_word(word, &is_command);

    char *slash = strrchr(word, '/');
    const char *prefix = slash ? slash + 1 : word;
    size_t dir_len = slash ? (size_t)(slash + 1 - word) : 0;

    clear_matches();
    if (is_command) {
        collect_dir_matches(BIN_DIR, prefix);
        for (const char *const *c = extra_commands; c && *c; c++) {
            if (strncmp(*c, prefix, strlen(prefix)) == 0)
                add_match(*c, false);
        }
    } else if (slash) {
        static char dir_path[MAX_LINE];

        memcpy(dir_path, word, dir_len);
        dir_path[dir_len] = '\0';
        collect_dir_matches(dir_path, prefix);
    } else {
        collect_dir_matches(".", prefix);
    }

    if (match_count == 0)
        return;

    // Rebuild the word from what was typed of its directory plus the match.
    size_t common = common_prefix_len();
    size_t used = escape_word(word, dir_len, replacement);

    used += escape_word(matches[0].name, common, &replacement[used]);
    if (match_count == 1)
        replacement[used++] = matches[0].is_dir ? '/' : ' ';

    bool unchanged = used == line_pos - start && memcmp(replacement, &line[start], used) == 0;
    if (!unchanged) {
        delete_range(start, line_pos);
        insert_text(replacement, used);
    } else if (match_count > 1 && repeated) {
        show_matches();
    }
}

// A signal, like a background job ending, isn't the end of the input.
static char read_key(void) {
    char c;
    ssize_t got;

    fflush(stdout);
    do
        got = read(STDIN_FILENO, &c, 1);
    while (got < 0 && errno == EINTR);
    return got == 1 ? c : CONTROL('d');
}

static void history_move(int *index, char *draft, int delta) {
    int target = *index + delta;

    if (target < 0 || target > history_count)
        return;
    if (*index == history_count)
        strcpy(draft, line);

    *index = target;
    set_line(target == history_count ? draft : history[target]);
}

// Handles the rest of an escape sequence, which the keyboard driver sends for
// the arrow keys and others.
// The keyboard delivers a whole sequence at once, so if nothing follows an escape
// byte, it was the Escape key.
static bool input_waiting(void) {
    struct pollfd in = {.fd = STDIN_FILENO, .events = POLLIN};

    return poll(&in, 1, 0) > 0;
}

// Alt+B and Alt+F move by words, Alt+D and Alt+Backspace delete them, and Ctrl
// with the left and right arrows moves by words too.
static void handle_escape(int *history_index, char *draft) {
    char c = read_key();

    if (c == 'b' || c == 'f')
        line_pos = c == 'b' ? word_before() : word_after();
    else if (c == 'd')
        delete_range(line_pos, word_after());
    else if (c == 0x7F || c == '\b')
        delete_range(word_before(), line_pos);
    if (c != '[')
        return;

    switch (c = read_key()) {
    case '1': // 1;5C and 1;5D, the arrows with Ctrl
        if (read_key() == ';' && read_key() == '5' && ((c = read_key()) == 'C' || c == 'D'))
            line_pos = c == 'D' ? word_before() : word_after();
        break;
    case 'A':
        history_move(history_index, draft, -1);
        break;
    case 'B':
        history_move(history_index, draft, 1);
        break;
    case 'C':
        if (line_pos < line_len)
            line_pos++;
        break;
    case 'D':
        if (line_pos > 0)
            line_pos--;
        break;
    case 'H':
        line_pos = 0;
        break;
    case 'F':
        line_pos = line_len;
        break;
    case '3':
        read_key(); // the ~ ending this sequence
        if (line_pos < line_len)
            delete_range(line_pos, line_pos + 1);
        break;
    }
}

// Ctrl+R searches back through the history for what's typed, and again for an older
// match. Enter runs the match, and any other key keeps it to edit, as does Escape.
// Ctrl+G and Ctrl+C go back to the line as it was. Returns whether to run it.
static bool search_history(int *history_index) {
    char query[64] = "", shown[96], kept[MAX_LINE];
    const char *was = prompt;
    size_t length = 0;
    int found = history_count;
    bool run = false;

    strcpy(kept, line);
    for (;;) {
        const char *match = found < history_count ? history[found] : "";

        snprintf(shown, sizeof(shown), "(search '%s'): ", query);
        prompt = shown;
        prompt_len = strlen(shown);
        set_line(match);
        refresh();

        char c = read_key();
        int from = found;

        if (c == CONTROL('r'))
            from--;
        else if ((c == 0x7F || c == '\b') && length)
            query[--length] = '\0', from = history_count;
        else if (c >= 0x20 && c < 0x7F && length < sizeof(query) - 1)
            query[length++] = c, query[length] = '\0', from = from < history_count ? from : history_count - 1;
        else if (c == CONTROL('g') || c == CONTROL('c')) {
            set_line(kept);
            break;
        } else {
            if (c == '\x1b')
                while (input_waiting())
                    read_key();
            run = c == '\n';
            *history_index = found;
            break;
        }
        // The newest match from there back that has what's typed.
        for (from = from < history_count ? from : history_count - 1; from >= 0; from--) {
            if (strstr(history[from], query))
                break;
        }
        if (from >= 0)
            found = from;
    }
    prompt = was;
    prompt_len = strlen(was);
    return run;
}

static bool edit(bool *interrupted) {
    static char draft[MAX_LINE];
    int history_index = history_count;
    bool last_was_tab = false;

    line[0] = '\0';
    line_len = line_pos = 0;
    cursor_row = 0;
    refresh();

    for (;;) {
        char c = read_key();
        bool is_tab = c == '\t';

        switch (c) {
        case '\n':
            line_pos = line_len;
            refresh();
            printf("\n");
            return true;
        case 0x7F:
        case '\b':
            if (line_pos > 0)
                delete_range(line_pos - 1, line_pos);
            break;
        case '\t':
            complete(last_was_tab);
            break;
        case '\x1b':
            if (input_waiting())
                handle_escape(&history_index, draft);
            break;
        case CONTROL('a'):
            line_pos = 0;
            break;
        case CONTROL('e'):
            line_pos = line_len;
            break;
        case CONTROL('c'):
            line_pos = line_len;
            refresh();
            printf("^C");
            *interrupted = true;
            return false;
        case CONTROL('d'):
            if (line_len == 0)
                return false;
            if (line_pos < line_len)
                delete_range(line_pos, line_pos + 1);
            break;
        case CONTROL('k'):
            delete_range(line_pos, line_len);
            break;
        case CONTROL('w'):
            delete_range(word_before(), line_pos);
            break;
        case CONTROL('r'):
            if (search_history(&history_index)) {
                refresh();
                printf("\n");
                return true;
            }
            break;
        case CONTROL('u'):
            delete_range(0, line_pos);
            break;
        case CONTROL('l'):
            printf("\x1b[H\x1b[2J");
            cursor_row = 0;
            break;
        default:
            if (c >= 0x20 && c < 0x7F)
                insert_text(&c, 1);
        }

        last_was_tab = is_tab;
        refresh();
    }
}

void line_history(const char *file) {
    history_file = file;
}

char *line_read(const char *text, const char *const *commands) {
    // What a program left behind doesn't count, so this is only read once.
    static struct termios settings;
    static bool have_settings;
    bool on_terminal = isatty(STDIN_FILENO);
    bool interrupted = false;
    int rows;

    static bool loaded;

    prompt = text;
    prompt_len = strlen(text);
    extra_commands = commands;
    if (!loaded) {
        load_history();
        loaded = true;
    }

    // A program may have changed the resolution.
    if (termsize(&rows, &screen_cols) < 0 || screen_cols < 1)
        screen_cols = 80;

    if (on_terminal) {
        if (!have_settings)
            have_settings = tcgetattr(STDIN_FILENO, &settings) == 0;

        struct termios raw = settings;

        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    bool got_line = edit(&interrupted);

    fflush(stdout);
    if (on_terminal)
        tcsetattr(STDIN_FILENO, TCSANOW, &settings);
    if (!got_line) {
        errno = interrupted ? EINTR : 0;
        return NULL;
    }
    if (history_add(line))
        save_history(line);
    return line;
}
