#pragma once

// Reads a line from the terminal with editing, history and Tab completion. Completion
// offers /usr/bin and commands, a NULL-terminated list that may be NULL. Returns the
// line without its newline, valid until the next call, or NULL with errno 0 at the end
// of the input or EINTR after Ctrl+C, which leaves the cursor at the end of the "^C".
char *line_read(const char *prompt, const char *const *commands);

// Keeps the history in this file in the home folder, rather than .history. Called
// before the first line_read.
void line_history(const char *file);
