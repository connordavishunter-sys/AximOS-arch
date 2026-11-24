#include <aximos.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLUMNS 24
#define ROWS 18
#define CELL 20
#define BAR 28 // the score, over the field
#define MAX_TURNS 3 // waiting, so quick presses aren't lost
#define KEY_W 0x11
#define KEY_A 0x1E
#define KEY_S 0x1F
#define KEY_D 0x20
#define KEY_P 0x19
#define KEY_Q 0x10
#define KEY_1 0x02
#define LEVELS 3

// A snake that grows as it eats, until it runs into a wall or itself. The arrows
// or WASD turn it, P, Space or Escape pauses, and Space or Enter starts again,
// at the speed 1, 2 or 3 chose, each speed with its own best score.
static const char *const level_names[LEVELS] = {"Slow", "Normal", "Fast"};
static const int start_ms[LEVELS] = {180, 140, 100}, fastest_ms[LEVELS] = {90, 60, 40};
struct point {
    int x, y;
};

static struct gui_window *window;
static struct point body[COLUMNS * ROWS], food, direction, turns[MAX_TURNS];
static int length, turn_count, score, best[LEVELS], level = 1, timer = -1;
static bool new_best;
static enum { READY, PLAYING, PAUSED, OVER } state;
static char best_path[GUI_PATH_MAX];
static uint32_t background, field, text_colour, dim_colour;

static void load_best(void) {
    const char *home = getenv("HOME");
    FILE *f;

    snprintf(best_path, sizeof(best_path), "%s/.snake-best", home && strcmp(home, "/") ? home : "");
    if ((f = fopen(best_path, "r"))) {
        for (int i = 0; i < LEVELS && fscanf(f, "%d", &best[i]) == 1; i++)
            ;
        fclose(f);
    }
}

static void save_best(void) {
    FILE *f = fopen(best_path, "w");

    if (f) {
        fprintf(f, "%d %d %d\n", best[0], best[1], best[2]);
        fclose(f);
    }
}

static bool on_snake(struct point p) {
    for (int i = 0; i < length; i++) {
        if (body[i].x == p.x && body[i].y == p.y)
            return true;
    }
    return false;
}

static void place_food(void) {
    do
        food = (struct point){rand() % COLUMNS, rand() % ROWS};
    while (on_snake(food));
}

// Faster as it grows.
static void set_speed(void) {
    int ms = start_ms[level] - score * 4;

    if (timer >= 0)
        gui_timer_stop(timer);
    timer = gui_timer_start(ms > fastest_ms[level] ? ms : fastest_ms[level], true);
}

static void start(void) {
    length = 3;
    for (int i = 0; i < length; i++)
        body[i] = (struct point){COLUMNS / 2 - i, ROWS / 2};
    direction = (struct point){1, 0};
    turn_count = score = 0;
    new_best = false;
    state = PLAYING;
    place_food();
    set_speed();
    printf("snake: start %s\n", level_names[level]);
}

static void stop(void) {
    state = OVER;
    gui_timer_stop(timer);
    timer = -1;
    if (score > best[level]) {
        best[level] = score;
        new_best = true;
        save_best();
    }
    printf("snake: game over %d\n", score);
}

static void step(void) {
    if (turn_count) {
        direction = turns[0];
        memmove(turns, turns + 1, --turn_count * sizeof(*turns));
    }

    struct point head = {body[0].x + direction.x, body[0].y + direction.y};
    bool eats = head.x == food.x && head.y == food.y;

    length -= !eats; // the tail moves on, unless it's growing, and so isn't in the way
    if (head.x < 0 || head.y < 0 || head.x >= COLUMNS || head.y >= ROWS || on_snake(head)) {
        length += !eats;
        stop();
        return;
    }
    memmove(body + 1, body, length * sizeof(*body));
    body[0] = head;
    length++;
    if (eats) {
        score++;
        printf("snake: score %d\n", score);
        if (length == COLUMNS * ROWS) {
            stop();
            return;
        }
        place_food();
        set_speed();
    }
}

// A turn waits its go, and is only ever to the side of the way it'll be going.
static void turn(int dx, int dy) {
    struct point last = turn_count ? turns[turn_count - 1] : direction;

    if (turn_count < MAX_TURNS && last.x * dx + last.y * dy == 0)
        turns[turn_count++] = (struct point){dx, dy};
}

static void key(const struct key_event *k) {
    uint16_t c = k->code;

    if (c == KEY_UP || c == KEY_W)
        turn(0, -1);
    else if (c == KEY_DOWN || c == KEY_S)
        turn(0, 1);
    else if (c == KEY_LEFT || c == KEY_A)
        turn(-1, 0);
    else if (c == KEY_RIGHT || c == KEY_D)
        turn(1, 0);
    if (state != PLAYING && state != PAUSED && c >= KEY_1 && c < KEY_1 + LEVELS) {
        level = c - KEY_1;
        printf("snake: speed %s\n", level_names[level]);
    }
    if (state != PLAYING && state != PAUSED && (c == KEY_SPACE || c == KEY_ENTER))
        start();
    else if ((state == PLAYING && c == KEY_ESCAPE) || ((state == PLAYING || state == PAUSED) && (c == KEY_P || c == KEY_SPACE))) {
        state = state == PLAYING ? PAUSED : PLAYING;
        printf("snake: %s\n", state == PAUSED ? "paused" : "playing");
    }
}

static void centred(struct gui_canvas *c, int y, const char *text, uint32_t colour, int scale) {
    gui_text(c, (c->width - gui_text_width(text, scale)) / 2, y, text, colour, scale);
}

static void draw(void) {
    struct gui_canvas *c = &window->canvas;
    int left = (c->width - COLUMNS * CELL) / 2, top = BAR + (c->height - BAR - ROWS * CELL) / 2;
    char line[64];

    gui_fill(c, 0, 0, c->width, c->height, background);
    snprintf(line, sizeof(line), "Score %d", score);
    gui_text(c, 10, (BAR - GUI_CHAR_HEIGHT) / 2, line, text_colour, 1);
    snprintf(line, sizeof(line), "%s  Best %d", level_names[level], best[level]);
    gui_text(c, c->width - 10 - gui_text_width(line, 1), (BAR - GUI_CHAR_HEIGHT) / 2, line, dim_colour, 1);
    gui_fill(c, left, top, COLUMNS * CELL, ROWS * CELL, field);
    if (state != READY)
        gui_fill(c, left + food.x * CELL + 4, top + food.y * CELL + 4, CELL - 8, CELL - 8, 0xE5484D);
    for (int i = length - 1; i >= 0 && state != READY; i--)
        gui_fill(c, left + body[i].x * CELL + 1, top + body[i].y * CELL + 1, CELL - 2, CELL - 2,
                 i ? 0x27AE60 : 0x6FCF97);

    int middle = top + ROWS * CELL / 2;

    if (state == READY)
        centred(c, middle - 40, "SNAKE", text_colour, 4);
    if (state == OVER)
        centred(c, middle - 40, new_best ? "NEW BEST!" : "GAME OVER", text_colour, 3);
    if (state == PAUSED)
        centred(c, middle - 16, "PAUSED", text_colour, 2);
    if (state == PAUSED)
        centred(c, middle + 20, "P or Space to go on", dim_colour, 1);
    if (state == READY || state == OVER) {
        snprintf(line, sizeof(line), "Speed: %s, 1 2 3 to change", level_names[level]);
        centred(c, middle + 30, "Space to start, arrows to turn", dim_colour, 1);
        centred(c, middle + 50, line, dim_colour, 1);
    }
    gui_show(window);
}

static void use_theme(void) {
    struct gui_settings settings;

    gui_settings_load(&settings);
    background = settings.light ? 0xF3F4F7 : 0x1E2129;
    field = settings.light ? 0xE2E5EB : 0x161A22;
    text_colour = settings.light ? 0x1E2129 : 0xE6E8EE;
    dim_colour = settings.light ? 0x6B7280 : 0x8A93A6;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: snake\n"
                            "A snake that grows as it eats. Arrows or WASD turn it, P pauses, 1 to 3 set the speed.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "snake: wm isn't running\n");
        return 1;
    }

    struct gui_message e;

    window = gui_create_window("Snake", COLUMNS * CELL + 20, BAR + ROWS * CELL + 10);
    if (!window) {
        dprintf(STDERR_FILENO, "snake: couldn't make a window\n");
        return 1;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    load_best();
    use_theme();
    draw();
    printf("snake: ready\n");
    fflush(stdout);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_KEY && e.key.pressed && e.key.code == KEY_Q && (e.key.modifiers & KEY_MOD_CTRL))
            break;
        if (e.type == GUI_KEY && e.key.pressed)
            key(&e.key);
        else if (e.type == GUI_TIMER && e.x == timer && state == PLAYING)
            step();
        else if (e.type == GUI_FOCUS && !e.focused && state == PLAYING)
            key(&(struct key_event){.code = KEY_P, .pressed = true}); // pauses while it's not looked at
        else if (e.type == GUI_SETTINGS)
            use_theme();
        else if (e.type == GUI_RESIZE)
            gui_resize(window, e.width, e.height);
        else if (e.type != GUI_TIMER && e.type != GUI_FOCUS)
            continue;
        draw();
        fflush(stdout);
    }
    gui_destroy_window(window);
    return 0;
}
