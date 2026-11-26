#include "system.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define INTERACTIVE_USEC (120ull * 1000000) // long enough to type a password

static sd_bus *bus;
static char error[256];

bool system_open(void) {
    return bus || sd_bus_open_system(&bus) >= 0;
}

const char *system_error(void) {
    return error;
}

static bool failed(int r, sd_bus_error *e) {
    if (r >= 0) {
        sd_bus_error_free(e);
        return false;
    }
    snprintf(error, sizeof(error), "%s",
             sd_bus_error_has_name(e, "org.freedesktop.PolicyKit1.Error.Cancelled")
                     || sd_bus_error_has_name(e, SD_BUS_ERROR_INTERACTIVE_AUTHORIZATION_REQUIRED)
                 ? "It needs an admin's password"
             : sd_bus_error_has_name(e, SD_BUS_ERROR_ACCESS_DENIED) ? "That isn't allowed"
             : e->message                                         ? e->message
                                                                  : strerror(-r));
    sd_bus_error_free(e);
    return true;
}

// A method, allowed to ask for a password, and given that long.
static bool ask(const char *service, const char *path, const char *iface, const char *method, const char *types, ...) {
    sd_bus_message *m = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    va_list args;
    int r;

    if (!system_open())
        return snprintf(error, sizeof(error), "There's no system bus"), false;
    r = sd_bus_message_new_method_call(bus, &m, service, path, iface, method);
    if (r >= 0)
        r = sd_bus_message_set_allow_interactive_authorization(m, true);
    va_start(args, types);
    if (r >= 0)
        r = sd_bus_message_appendv(m, types, args);
    va_end(args);
    if (r >= 0)
        r = sd_bus_call(bus, m, INTERACTIVE_USEC, &e, NULL);
    sd_bus_message_unref(m);
    return !failed(r, &e);
}

static bool get_string(const char *service, const char *path, const char *iface, const char *name, char *out, size_t size) {
    char *value = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = bus ? sd_bus_get_property_string(bus, service, path, iface, name, &e, &value) : -1;

    sd_bus_error_free(&e);
    if (r >= 0)
        snprintf(out, size, "%s", value);
    free(value);
    return r >= 0;
}

static bool get(const char *service, const char *path, const char *iface, const char *name, char type, void *out) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = bus ? sd_bus_get_property_trivial(bus, service, path, iface, name, &e, type, out) : -1;

    sd_bus_error_free(&e);
    return r >= 0;
}

// The battery, as UPower's display device sums them, and the first battery's health.
#define UPOWER "org.freedesktop.UPower"
#define DEVICE UPOWER ".Device"

bool battery_read(struct battery *b) {
    static const char *const states[] = {"", "charging", "discharging", "empty", "full", "not charging", "discharging"};
    const char *display = "/org/freedesktop/UPower/devices/DisplayDevice";
    uint32_t type = 0, state = 0;
    int64_t empty = 0, full = 0;
    char **paths = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;

    *b = (struct battery){0};
    if (!system_open() || !get(UPOWER, display, DEVICE, "Type", 'u', &type) || type != 2) // 2 is a battery
        return false;
    b->present = true;
    get(UPOWER, display, DEVICE, "Percentage", 'd', &b->percent);
    get(UPOWER, display, DEVICE, "State", 'u', &state);
    get(UPOWER, display, DEVICE, "TimeToEmpty", 'x', &empty);
    get(UPOWER, display, DEVICE, "TimeToFull", 'x', &full);
    snprintf(b->state, sizeof(b->state), "%s", state < 7 ? states[state] : "");
    b->seconds_left = state == 1 ? full : empty;

    if (sd_bus_call_method(bus, UPOWER, "/org/freedesktop/UPower", UPOWER, "EnumerateDevices", &e, &reply, "") >= 0
        && sd_bus_message_read_strv(reply, &paths) >= 0) {
        for (char **p = paths; p && *p; p++) {
            bool power_supply = false;

            if (get(UPOWER, *p, DEVICE, "Type", 'u', &type) && type == 2 && get(UPOWER, *p, DEVICE, "PowerSupply", 'b', &power_supply)
                && power_supply) {
                double now = 0, design = 0;
                int32_t cycles = 0;
                char vendor[32] = "", model[32] = "";

                get(UPOWER, *p, DEVICE, "EnergyFull", 'd', &now);
                get(UPOWER, *p, DEVICE, "EnergyFullDesign", 'd', &design);
                get(UPOWER, *p, DEVICE, "ChargeCycles", 'i', &cycles);
                get_string(UPOWER, *p, DEVICE, "Vendor", vendor, sizeof(vendor));
                get_string(UPOWER, *p, DEVICE, "Model", model, sizeof(model));
                b->health = design > 0 ? now * 100 / design : 0;
                b->cycles = cycles > 0 ? (uint32_t)cycles : 0;
                snprintf(b->model, sizeof(b->model), "%s%s%s", vendor, vendor[0] && model[0] ? " " : "", model);
                break;
            }
        }
    }
    for (char **p = paths; p && *p; p++)
        free(*p);
    free(paths);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
    return true;
}

// power-profiles-daemon, as it's named now, and as it was.
#define PROFILES "org.freedesktop.UPower.PowerProfiles"
#define PROFILES_PATH "/org/freedesktop/UPower/PowerProfiles"

bool power_modes_read(struct power_modes *m) {
    sd_bus_message *reply = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;

    *m = (struct power_modes){0};
    if (!system_open() || !get_string(PROFILES, PROFILES_PATH, PROFILES, "ActiveProfile", m->active, sizeof(m->active)))
        return false;
    m->available = true;
    if (sd_bus_get_property(bus, PROFILES, PROFILES_PATH, PROFILES, "Profiles", &e, &reply, "aa{sv}") >= 0
        && sd_bus_message_enter_container(reply, 'a', "a{sv}") >= 0) {
        while (m->count < 3 && sd_bus_message_enter_container(reply, 'a', "{sv}") > 0) {
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                const char *key, *value;

                sd_bus_message_read(reply, "s", &key);
                if (!strcmp(key, "Profile") && sd_bus_message_read(reply, "v", "s", &value) >= 0)
                    snprintf(m->names[m->count], sizeof(m->names[0]), "%s", value);
                else
                    sd_bus_message_skip(reply, "v");
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
            m->count++;
        }
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
    return true;
}

bool power_mode_set(const char *name) {
    sd_bus_error e = SD_BUS_ERROR_NULL;

    return system_open() && !failed(sd_bus_set_property(bus, PROFILES, PROFILES_PATH, PROFILES, "ActiveProfile", &e, "s", name), &e);
}

#define TIMEDATE "org.freedesktop.timedate1"
#define TIMEDATE_PATH "/org/freedesktop/timedate1"

bool clock_read(struct clock_state *c) {
    int ntp = 0, can = 0, synced = 0;

    *c = (struct clock_state){0};
    if (!system_open() || !get(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "NTP", 'b', &ntp))
        return false;
    get(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "CanNTP", 'b', &can);
    get(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "NTPSynchronized", 'b', &synced);
    get_string(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "Timezone", c->zone, sizeof(c->zone));
    c->ntp = ntp, c->can_ntp = can, c->synced = synced;
    return true;
}

bool clock_set_ntp(bool on) {
    return ask(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "SetNTP", "bb", (int)on, 1);
}

bool clock_set(int64_t seconds) {
    return ask(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "SetTime", "xbb", seconds * 1000000, 0, 1);
}

bool clock_set_zone(const char *zone) {
    return ask(TIMEDATE, TIMEDATE_PATH, TIMEDATE, "SetTimezone", "sb", zone, 1);
}

#define HOSTNAME "org.freedesktop.hostname1"
#define HOSTNAME_PATH "/org/freedesktop/hostname1"

bool hostname_set(const char *name) {
    return ask(HOSTNAME, HOSTNAME_PATH, HOSTNAME, "SetStaticHostname", "sb", name, 1);
}

// The first line of a file starting with key, after it and its colon.
static void field(const char *path, const char *key, char *out, size_t size) {
    FILE *f = fopen(path, "r");
    char line[512];

    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, strlen(key))) {
            char *v = strchr(line, ':');

            v = v ? v + 1 : line + strlen(key);
            v += strspn(v, " \t=\"");
            v[strcspn(v, "\"\n")] = '\0';
            snprintf(out, size, "%s", v);
            break;
        }
    }
    if (f)
        fclose(f);
}

// A graphics card's name from hwdata's list of PCI ids, or its vendor's.
static void gpu_name(unsigned vendor, unsigned device, char *out, size_t size) {
    FILE *f = fopen("/usr/share/hwdata/pci.ids", "r");
    char line[256], vendor_name[96] = "";
    bool in_vendor = false;

    while (f && fgets(line, sizeof(line), f)) {
        unsigned id;

        line[strcspn(line, "\n")] = '\0';
        if (line[0] != '\t' && line[0] != '#' && sscanf(line, "%4x", &id) == 1) {
            if (in_vendor)
                break;
            if ((in_vendor = id == vendor))
                snprintf(vendor_name, sizeof(vendor_name), "%s", line + 6);
        } else if (in_vendor && line[0] == '\t' && line[1] != '\t' && sscanf(line + 1, "%4x", &id) == 1 && id == device) {
            snprintf(out, size, "%s %s", vendor_name, line + 7);
            fclose(f);
            return;
        }
    }
    if (f)
        fclose(f);
    snprintf(out, size, "%s", vendor_name[0] ? vendor_name : vendor == 0x10de ? "NVIDIA" : vendor == 0x8086 ? "Intel"
                                                           : vendor == 0x1002 ? "AMD" : "Graphics card");
}

void about_read(struct about *a) {
    struct utsname u;
    struct statvfs fs;
    char memory[32] = "", vendor[48] = "", product[64] = "";

    *a = (struct about){0};
    gethostname(a->hostname, sizeof(a->hostname) - 1);
    field("/etc/os-release", "PRETTY_NAME", a->os, sizeof(a->os));
    if (uname(&u) == 0)
        snprintf(a->kernel, sizeof(a->kernel), "Linux %s", u.release);
    field("/proc/cpuinfo", "model name", a->cpu, sizeof(a->cpu));
    field("/proc/meminfo", "MemTotal", memory, sizeof(memory));
    a->memory = strtoull(memory, NULL, 10) * 1024;
    if (statvfs("/", &fs) == 0)
        a->disk_total = (uint64_t)fs.f_blocks * fs.f_frsize, a->disk_free = (uint64_t)fs.f_bavail * fs.f_frsize;
    field("/sys/class/dmi/id/sys_vendor", "", vendor, sizeof(vendor));
    field("/sys/class/dmi/id/product_version", "", product, sizeof(product));
    if (!product[0] || !strcmp(product, "None"))
        field("/sys/class/dmi/id/product_name", "", product, sizeof(product));
    snprintf(a->model, sizeof(a->model), "%s%s%s", vendor, vendor[0] ? " " : "", product);

    // Graphics cards, display controllers among the PCI devices.
    for (int i = 0; i < 64 && a->gpus < 2; i++) {
        char path[96], value[16] = "";
        unsigned klass, vendor_id, device_id;
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/class", i);
        if (!(f = fopen(path, "r")))
            continue;
        if (!fgets(value, sizeof(value), f) || sscanf(value, "%x", &klass) != 1 || klass >> 16 != 0x03) {
            fclose(f);
            continue;
        }
        fclose(f);
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/vendor", i);
        field(path, "", value, sizeof(value));
        vendor_id = (unsigned)strtoul(value, NULL, 16);
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/device", i);
        field(path, "", value, sizeof(value));
        device_id = (unsigned)strtoul(value, NULL, 16);
        gpu_name(vendor_id, device_id, a->gpu[a->gpus++], sizeof(a->gpu[0]));
    }
}
