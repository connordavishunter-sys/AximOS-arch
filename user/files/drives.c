#include "drives.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

// udisks2, over D-Bus with sd-bus, polled with Files' window. Any signal it sends
// has the drives asked for again.

#define UDISKS "org.freedesktop.UDisks2"
#define UDISKS_PATH "/org/freedesktop/UDisks2"
#define BLOCK UDISKS ".Block"
#define FILESYSTEM UDISKS ".Filesystem"
#define DRIVE UDISKS ".Drive"

struct drive drives[DRIVES_MAX];
int drive_count;

static sd_bus *bus;
static bool stale;

static int on_signal(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)m, (void)data, (void)e;
    stale = true;
    return 0;
}

// A property's value, if it's key and of type, from a {sv} entered already.
static bool value_of(sd_bus_message *m, const char *key, const char *wanted, const char *type, void *value) {
    if (strcmp(key, wanted))
        return false;
    sd_bus_message_enter_container(m, 'v', type);
    sd_bus_message_read_basic(m, type[0], value);
    sd_bus_message_exit_container(m);
    return true;
}

// The first place a filesystem's mounted, from its MountPoints, an array of paths as bytes.
static void read_mount(sd_bus_message *m, char *mount) {
    const void *path;
    size_t length;

    sd_bus_message_enter_container(m, 'v', "aay");
    sd_bus_message_enter_container(m, 'a', "ay");
    while (sd_bus_message_read_array(m, 'y', &path, &length) > 0) {
        if (!mount[0] && length > 1)
            snprintf(mount, PATH_MAX, "%.*s", (int)length, (const char *)path);
    }
    sd_bus_message_exit_container(m);
    sd_bus_message_exit_container(m);
}

// What's plugged in: each filesystem that's not the system's own, nor hidden.
static void read_drives(void) {
    sd_bus_message *reply = NULL;
    int count = 0;

    stale = false;
    if (!bus || sd_bus_call_method(bus, UDISKS, UDISKS_PATH, "org.freedesktop.DBus.ObjectManager",
                                   "GetManagedObjects", NULL, &reply, "") < 0) {
        drive_count = 0;
        return;
    }
    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        struct drive d = {0};
        const char *path, *label = "", *on = "";
        uint64_t bytes = 0;
        int system = 1, ignore = 0;
        bool filesystem = false;

        sd_bus_message_read(reply, "o", &path);
        snprintf(d.block, sizeof(d.block), "%s", path);
        sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface, *key;

            sd_bus_message_read(reply, "s", &iface);
            sd_bus_message_enter_container(reply, 'a', "{sv}");
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                sd_bus_message_read(reply, "s", &key);
                if (!strcmp(iface, BLOCK)
                    && (value_of(reply, key, "IdLabel", "s", &label) || value_of(reply, key, "Size", "t", &bytes)
                        || value_of(reply, key, "HintSystem", "b", &system)
                        || value_of(reply, key, "HintIgnore", "b", &ignore) || value_of(reply, key, "Drive", "o", &on)))
                    ;
                else if (!strcmp(iface, FILESYSTEM) && !strcmp(key, "MountPoints"))
                    read_mount(reply, d.mount);
                else
                    sd_bus_message_skip(reply, "v");
                sd_bus_message_exit_container(reply);
            }
            filesystem |= !strcmp(iface, FILESYSTEM);
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        // Those mounted elsewhere than udisks2 mounts them are the system's, such as the
        // live system's own stick or disc.
        if (!filesystem || system || ignore || count == DRIVES_MAX || (d.mount[0] && strncmp(d.mount, "/run/media/", 11)))
            continue;
        snprintf(d.drive, sizeof(d.drive), "%s", on);
        if (label[0])
            snprintf(d.name, sizeof(d.name), "%s", label);
        else
            snprintf(d.name, sizeof(d.name), "%.1f GB drive", bytes / 1e9);
        drives[count++] = d;
    }
    sd_bus_message_unref(reply);
    drive_count = count;
}

bool drives_open(void) {
    if (sd_bus_open_system(&bus) < 0) {
        bus = NULL;
        return false;
    }
    sd_bus_match_signal(bus, NULL, UDISKS, NULL, "org.freedesktop.DBus.ObjectManager", NULL, on_signal, NULL);
    sd_bus_match_signal(bus, NULL, UDISKS, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", on_signal, NULL);
    read_drives();
    return true;
}

struct pollfd drives_poll(int *timeout) {
    uint64_t usec = UINT64_MAX;

    *timeout = -1;
    if (!bus)
        return (struct pollfd){.fd = -1};
    if (sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
        struct timespec t;
        int64_t left;

        clock_gettime(CLOCK_MONOTONIC, &t);
        left = (int64_t)(usec / 1000) - ((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
        *timeout = left > 0 ? (int)left : 0;
    }
    return (struct pollfd){.fd = sd_bus_get_fd(bus), .events = sd_bus_get_events(bus)};
}

bool drives_read(void) {
    struct drive was[DRIVES_MAX];
    int was_count = drive_count;

    while (bus && sd_bus_process(bus, NULL) > 0)
        ;
    if (!stale)
        return false;
    memcpy(was, drives, sizeof(was));
    read_drives();
    return drive_count != was_count || memcmp(was, drives, sizeof(drives[0]) * drive_count);
}

// A method of udisks2's taking only options, none of them given, saying why it failed.
static bool ask(const char *path, const char *iface, const char *method, sd_bus_message **reply, char *error,
                size_t size) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, UDISKS, path, iface, method, &e, reply, "a{sv}", 0);

    if (r < 0 && error)
        snprintf(error, size, "%s", e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return r >= 0;
}

bool drive_mount(struct drive *d, char *error, size_t size) {
    sd_bus_message *reply = NULL;
    const char *path;

    if (d->mount[0])
        return true;
    if (!bus || !ask(d->block, FILESYSTEM, "Mount", &reply, error, size))
        return false;
    if (sd_bus_message_read(reply, "s", &path) >= 0)
        snprintf(d->mount, sizeof(d->mount), "%s", path);
    sd_bus_message_unref(reply);
    return d->mount[0];
}

bool drive_eject(const struct drive *d, char *error, size_t size) {
    if (!bus)
        return false;
    for (int i = 0; i < drive_count; i++) {
        if ((drives[i].drive[0] ? !strcmp(drives[i].drive, d->drive) : !strcmp(drives[i].block, d->block))
            && drives[i].mount[0] && !ask(drives[i].block, FILESYSTEM, "Unmount", NULL, error, size))
            return false;
    }
    // A USB stick's powered off, which a disc can't be, so it's ejected.
    if (d->drive[0] && strcmp(d->drive, "/") && !ask(d->drive, DRIVE, "PowerOff", NULL, NULL, 0))
        ask(d->drive, DRIVE, "Eject", NULL, NULL, 0);
    stale = true;
    return true;
}
