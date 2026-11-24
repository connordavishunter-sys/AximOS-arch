#include <aximos.h>
#include <gui.h>
#include <stdio.h>
#include <unistd.h>

#define WIDTH 360
#define HEIGHT 200
#define BACKGROUND 0xF4F1EA
#define INK 0x26282E
#define ACCENT 0x3D63DD

static int clicks;
static char last_key[32] = "none";
static bool focused;

static void draw(struct gui_window *w) {
    struct gui_canvas *c = &w->canvas;
    char line[64];

    gui_fill(c, 0, 0, c->width, c->height, BACKGROUND);
    gui_fill(c, 0, 0, c->width, 6, focused ? ACCENT : 0xB8BCC6);
    gui_text(c, 20, 28, "Hello from AximOS", INK, 2);
    snprintf(line, sizeof(line), "clicks: %d", clicks);
    gui_text(c, 20, 80, line, INK, 1);
    snprintf(line, sizeof(line), "last key: %s", last_key);
    gui_text(c, 20, 104, line, INK, 1);
    snprintf(line, sizeof(line), "%dx%d", c->width, c->height);
    gui_text(c, 20, 128, line, 0x6B7080, 1);
    gui_text(c, 20, c->height - 32, "Click, type, or Esc to close", 0x6B7080, 1);
    gui_show(w);
}

// A window to try the window system with. It prints what it gets, for the tests.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: guidemo\n"
                            "Opens a window that counts clicks and shows the last key pressed.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "guidemo: wm isn't running\n");
        return 1;
    }

    struct gui_window *w = gui_create_window("Demo", WIDTH, HEIGHT);
    struct gui_message e;

    if (!w) {
        dprintf(STDERR_FILENO, "guidemo: couldn't make a window\n");
        return 1;
    }
    draw(w);
    printf("guidemo: window %u\n", w->id);
    fflush(stdout);

    while (gui_next_event(&e)) {
        if (e.type == GUI_CLOSE || (e.type == GUI_KEY && e.key.pressed && e.key.code == KEY_ESCAPE))
            break;
        if (e.type == GUI_MOUSE_BUTTON && (e.changed & e.buttons)) {
            clicks++;
            printf("guidemo: click %d at %d,%d\n", clicks, e.x, e.y);
        } else if (e.type == GUI_KEY && e.key.pressed && e.key.text >= ' ' && e.key.text < 127) {
            snprintf(last_key, sizeof(last_key), "%c", (char)e.key.text);
            printf("guidemo: key %s\n", last_key);
        } else if (e.type == GUI_RESIZE && gui_resize(w, e.width, e.height)) {
            printf("guidemo: size %d %d\n", e.width, e.height);
        } else if (e.type == GUI_FOCUS) {
            focused = e.focused;
            printf("guidemo: focus %u\n", e.focused);
        } else {
            continue;
        }
        fflush(stdout);
        draw(w);
    }
    printf("guidemo: closed\n");
    gui_destroy_window(w);
    return 0;
}
