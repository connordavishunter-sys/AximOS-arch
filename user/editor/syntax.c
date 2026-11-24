#include "syntax.h"
#include <string.h>

static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool listed(const char *word, size_t length, const char *const *list) {
    for (; *list; list++) {
        if (strlen(*list) == length && !strncmp(*list, word, length))
            return true;
    }
    return false;
}

// Marks count characters from at as kind, returning where they end.
static size_t mark(uint8_t *kinds, size_t at, size_t count, uint8_t kind) {
    memset(kinds + at, kind, count);
    return at + count;
}

// A quoted string from at, to its closing quote or the end of the line, with
// backslashes escaping what's after them if escapes.
static size_t quoted(const char *s, size_t at, bool escapes) {
    char quote = s[at++];

    while (s[at] && s[at] != quote && s[at] != '\n')
        at += escapes && s[at] == '\\' && s[at + 1] ? 2 : 1;
    return s[at] == quote ? at + 1 : at;
}

// A number, or a word, from at.
static size_t word_end(const char *s, size_t at) {
    while (is_word(s[at]) || (s[at] == '.' && is_digit(s[at - 1])))
        at++;
    return at;
}

static void highlight_c(const char *s, uint8_t *kinds) {
    static const char *const keywords[] = {
        "break", "case", "const", "continue", "default", "do", "else", "enum", "extern", "for", "goto", "if",
        "inline", "register", "return", "sizeof", "static", "struct", "switch", "typedef", "union", "volatile",
        "while", "true", "false", "NULL", NULL,
    };
    static const char *const types[] = {
        "bool", "char", "double", "float", "int", "long", "short", "signed", "unsigned", "void", NULL,
    };
    bool line_start = true; // nothing but spaces yet on this line

    for (size_t at = 0, end; s[at]; at = end) {
        char c = s[at];

        if (c == '/' && s[at + 1] == '/') {
            end = at + strcspn(s + at, "\n");
            mark(kinds, at, end - at, GUI_SYNTAX_COMMENT);
        } else if (c == '/' && s[at + 1] == '*') {
            const char *close = strstr(s + at + 2, "*/");

            end = close ? (size_t)(close - s) + 2 : at + strlen(s + at);
            mark(kinds, at, end - at, GUI_SYNTAX_COMMENT);
        } else if (c == '#' && line_start) {
            end = at + strcspn(s + at, "\n");
            mark(kinds, at, end - at, GUI_SYNTAX_DIRECTIVE);
        } else if (c == '"' || c == '\'') {
            end = quoted(s, at, true);
            mark(kinds, at, end - at, GUI_SYNTAX_STRING);
        } else if (is_word(c)) {
            end = word_end(s, at);
            mark(kinds, at, end - at,
                 is_digit(c)                                                      ? GUI_SYNTAX_NUMBER
                 : listed(s + at, end - at, keywords)                             ? GUI_SYNTAX_KEYWORD
                 : listed(s + at, end - at, types) || (end - at > 2 && !strncmp(s + end - 2, "_t", 2)) ? GUI_SYNTAX_TYPE
                                                                                  : GUI_SYNTAX_PLAIN);
        } else {
            end = mark(kinds, at, 1, GUI_SYNTAX_PLAIN);
        }
        for (size_t i = at; i < end; i++)
            line_start = s[i] == '\n' || (line_start && (s[i] == ' ' || s[i] == '\t'));
    }
}

static void highlight_shell(const char *s, uint8_t *kinds) {
    static const char *const keywords[] = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in",
        "function", "return", "export", "local", "readonly", "shift", "exit", "break", "continue", NULL,
    };

    for (size_t at = 0, end; s[at]; at = end) {
        char c = s[at];
        bool word_start = at == 0 || !is_word(s[at - 1]);

        if (c == '#' && (at == 0 || strchr(" \t\n;", s[at - 1]))) {
            end = at + strcspn(s + at, "\n");
            mark(kinds, at, end - at, GUI_SYNTAX_COMMENT);
        } else if (c == '"' || c == '\'') {
            end = quoted(s, at, c == '"');
            mark(kinds, at, end - at, GUI_SYNTAX_STRING);
        } else if (c == '$' && s[at + 1] == '{') {
            end = at + strcspn(s + at, "}\n");
            end += s[end] == '}';
            mark(kinds, at, end - at, GUI_SYNTAX_TYPE);
        } else if (c == '$') {
            end = is_word(s[at + 1]) ? word_end(s, at + 1) : at + 1 + (s[at + 1] && strchr("?#@*!$-", s[at + 1]));
            mark(kinds, at, end - at, GUI_SYNTAX_TYPE);
        } else if (is_word(c) && word_start) {
            end = word_end(s, at);
            mark(kinds, at, end - at,
                 is_digit(c) && end == at + strspn(s + at, "0123456789") ? GUI_SYNTAX_NUMBER
                 : listed(s + at, end - at, keywords)                      ? GUI_SYNTAX_KEYWORD
                                                                           : GUI_SYNTAX_PLAIN);
        } else {
            end = mark(kinds, at, 1, GUI_SYNTAX_PLAIN);
        }
    }
}

static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);

    return n >= m && !strcmp(s + n - m, end);
}

gui_highlighter *syntax_for(const char *path, const char *text) {
    size_t first_line = strcspn(text, "\n");

    if (ends_with(path, ".c") || ends_with(path, ".h"))
        return highlight_c;
    if (ends_with(path, ".sh") || (!strncmp(text, "#!", 2) && memmem(text, first_line, "sh", 2)))
        return highlight_shell;
    return NULL;
}

const char *syntax_name(gui_highlighter *highlight) {
    return highlight == highlight_c ? "C" : highlight == highlight_shell ? "Shell" : "Plain text";
}
