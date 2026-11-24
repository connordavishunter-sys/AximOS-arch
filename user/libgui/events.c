#include "backend.h"
#include <time.h>

// The events a program gets: those waiting, then timers that are due, then whatever
// the backend brings, the windows' own and those posted while waiting for answers.

#define QUEUE_SIZE 64

static struct gui_message queue[QUEUE_SIZE];
static int queue_head, queue_count;
static struct gui_screen screen = {.scale = 1};

static struct timer {
    int id, ms; // id 0 when it isn't running
    bool repeat;
    int64_t due;
} timers[GUI_MAX_TIMERS];
static int last_timer;

void gui_post(const struct gui_message *event) {
    if (queue_count < QUEUE_SIZE)
        queue[(queue_head + queue_count++) % QUEUE_SIZE] = *event;
}

int gui_queued(void) {
    return queue_count;
}

const struct gui_screen *gui_screen(void) {
    return &screen;
}

int64_t gui_now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int gui_timer_start(int ms, bool repeat) {
    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (!timers[i].id) {
            timers[i] = (struct timer){++last_timer, ms > 0 ? ms : 1, repeat, gui_now_ms() + ms};
            return timers[i].id;
        }
    }
    return -1;
}

void gui_timer_stop(int timer) {
    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (timers[i].id == timer)
            timers[i].id = 0;
    }
}

// The timer due soonest, or NULL when none are running.
static struct timer *next_timer(void) {
    struct timer *next = NULL;

    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (timers[i].id && (!next || timers[i].due < next->due))
            next = &timers[i];
    }
    return next;
}

// When to stop waiting: the next timer, or until if that's sooner or there's none.
static int64_t next_due(int64_t until) {
    struct timer *t = next_timer();

    return t && (until < 0 || t->due < until) ? t->due : until;
}

// Fills in event and returns true if a timer is due.
static bool timer_due(struct gui_message *event) {
    struct timer *t = next_timer();
    int64_t now = gui_now_ms();

    if (!t || t->due > now)
        return false;
    *event = (struct gui_message){.type = GUI_TIMER, .x = t->id};
    if (!t->repeat)
        t->id = 0;
    t->due = t->due + t->ms > now ? t->due + t->ms : now + t->ms;
    return true;
}

int gui_due_ms(void) {
    int64_t due = next_due(gui_backend_due()), now = gui_now_ms();

    return due < 0 ? -1 : due > now ? (int)(due - now) : 0;
}

int gui_wait_event(struct gui_message *event, int ms) {
    int64_t until = ms < 0 ? -1 : gui_now_ms() + ms;

    // What's arrived is always read once, even when there's no time to wait.
    for (bool read_once = false;; read_once = true) {
        if (queue_count > 0) {
            *event = queue[queue_head];
            queue_head = (queue_head + 1) % QUEUE_SIZE;
            queue_count--;
            if (event->type == GUI_SCREEN)
                screen = (struct gui_screen){event->width, event->height, event->x == 1, event->y > 1 ? event->y : 1};
            return 1;
        }
        if (timer_due(event))
            return 1;
        if (read_once && until >= 0 && gui_now_ms() >= until)
            return 0;
        if (!gui_backend_wait(next_due(until)))
            return -1;
    }
}

bool gui_next_event(struct gui_message *event) {
    return gui_wait_event(event, -1) == 1;
}
