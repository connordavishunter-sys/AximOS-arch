#include "network.h"
#include <arpa/inet.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

// NetworkManager, over D-Bus with sd-bus, on wm's own poll loop. Any signal it sends
// marks what's known as stale, and it's all asked for again a little later, so a
// scan's many changes come as one.

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define DEVICE NM ".Device"
#define WIRELESS NM ".Device.Wireless"
#define ACCESS_POINT NM ".AccessPoint"
#define SETTINGS_PATH NM_PATH "/Settings"
#define SETTINGS NM ".Settings"
#define CONNECTION NM ".Settings.Connection"
#define IP4 NM ".IP4Config"
#define REFRESH_MS 300 // after a change, before asking again
#define JOIN_MS 45000  // the longest joining can take

// NetworkManager's numbers for kinds of device, their states, and why they failed.
enum { DEVICE_ETHERNET = 1, DEVICE_WIFI = 2 };
enum { STATE_UNAVAILABLE = 20, STATE_DISCONNECTED = 30, STATE_PREPARE = 40, STATE_NEED_AUTH = 60, STATE_ACTIVATED = 100,
       STATE_FAILED = 120 };
enum { REASON_NO_SECRETS = 7, REASON_SUPPLICANT_TIMEOUT = 11 };
#define AP_PRIVACY 0x1
#define KEY_MGMT_SAE 0x400

static sd_bus *bus;
static struct network now;
static bool stale = true;
static int64_t changed_at;
static char error[128];

// A saved connection's path, by the network it's for.
struct saved {
    char ssid[SSID_MAX], path[128];
};
static struct saved saved[WIFI_MAX];
static int saved_count;
static char wifi_device[128], wired_device[128]; // their paths, or ""
static int wifi_state, wifi_reason;

// Joining: whether it's got anywhere yet, and the connection made for it, which goes
// again if joining fails.
static int64_t join_started;
static bool join_progressed;
static char join_made[128];

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int fail_reason; // why the Wi-Fi card last failed, while joining, as it said then

static int on_signal(sd_bus_message *m, void *data, sd_bus_error *e) {
    uint32_t state, old, reason;

    (void)data, (void)e;
    // Failing is said as it happens, as the card's back to disconnected soon after, which says no more why.
    // Asking for the password again, which only a wrong one does here, is as good as saying so.
    if (now.joining[0] && sd_bus_message_is_signal(m, DEVICE, "StateChanged")
        && !strcmp(sd_bus_message_get_path(m), wifi_device) && sd_bus_message_read(m, "uuu", &state, &old, &reason) >= 0) {
        if (state == STATE_NEED_AUTH)
            fail_reason = REASON_NO_SECRETS;
        else if ((state == STATE_FAILED || state == STATE_DISCONNECTED) && reason && !fail_reason)
            fail_reason = reason;
    }
    if (!stale)
        changed_at = now_ms();
    stale = true;
    return 0;
}

// Only NetworkManager's own coming and going, of everything on the bus.
static int on_owner(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *name;

    return sd_bus_message_read(m, "s", &name) >= 0 && !strcmp(name, NM) ? on_signal(m, data, e) : 0;
}

bool network_open(void) {
    if (sd_bus_open_system(&bus) < 0) {
        bus = NULL;
        return false;
    }
    sd_bus_match_signal(bus, NULL, NM, NULL, NULL, NULL, on_signal, NULL);
    sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner, NULL);
    return true;
}

static uint32_t get_u32(const char *path, const char *iface, const char *name) {
    uint32_t value = 0;

    sd_bus_get_property_trivial(bus, NM, path, iface, name, NULL, 'u', &value);
    return value;
}

static bool get_bool(const char *path, const char *iface, const char *name) {
    int value = 0;

    sd_bus_get_property_trivial(bus, NM, path, iface, name, NULL, 'b', &value);
    return value;
}

// A string or object path property, into out.
static void get_string(const char *path, const char *iface, const char *name, char type, char *out, size_t size) {
    sd_bus_message *reply = NULL;
    const char *value = NULL;
    char signature[2] = {type, 0};

    out[0] = '\0';
    if (sd_bus_get_property(bus, NM, path, iface, name, NULL, &reply, signature) >= 0
        && sd_bus_message_read_basic(reply, type, &value) > 0 && value)
        snprintf(out, size, "%s", value);
    sd_bus_message_unref(reply);
}

// Reads bytes, as Wi-Fi networks' names are, into a string.
static void read_ssid(sd_bus_message *m, char ssid[SSID_MAX]) {
    const void *bytes = NULL;
    size_t size = 0;

    ssid[0] = '\0';
    if (sd_bus_message_read_array(m, 'y', &bytes, &size) >= 0 && bytes) {
        size = size < SSID_MAX - 1 ? size : SSID_MAX - 1;
        memcpy(ssid, bytes, size);
        ssid[size] = '\0';
    }
}

// A list of object paths, from a method, each given to each.
static void for_each_path(const char *path, const char *iface, const char *method, void (*each)(const char *path)) {
    sd_bus_message *reply = NULL;
    const char *item;

    if (sd_bus_call_method(bus, NM, path, iface, method, NULL, &reply, "") < 0)
        return;
    if (sd_bus_message_enter_container(reply, 'a', "o") > 0) {
        while (sd_bus_message_read(reply, "o", &item) > 0)
            each(item);
    }
    sd_bus_message_unref(reply);
}

static void add_device(const char *path) {
    uint32_t type = get_u32(path, DEVICE, "DeviceType");

    if (type == DEVICE_WIFI && !wifi_device[0])
        snprintf(wifi_device, sizeof(wifi_device), "%s", path);
    else if (type == DEVICE_ETHERNET && !wired_device[0])
        snprintf(wired_device, sizeof(wired_device), "%s", path);
}

// Saved Wi-Fi networks: the settings of each connection, a{sa{sv}}, for its type and network.
static void add_saved(const char *path) {
    sd_bus_message *reply = NULL;
    const char *group, *key, *type = "";
    char ssid[SSID_MAX] = "";

    if (saved_count == WIFI_MAX || sd_bus_call_method(bus, NM, path, CONNECTION, "GetSettings", NULL, &reply, "") < 0)
        return;
    sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
        sd_bus_message_read(reply, "s", &group);
        sd_bus_message_enter_container(reply, 'a', "{sv}");
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
            sd_bus_message_read(reply, "s", &key);
            if (!strcmp(group, "connection") && !strcmp(key, "type")) {
                sd_bus_message_enter_container(reply, 'v', "s");
                sd_bus_message_read(reply, "s", &type);
                sd_bus_message_exit_container(reply);
            } else if (!strcmp(group, "802-11-wireless") && !strcmp(key, "ssid")) {
                sd_bus_message_enter_container(reply, 'v', "ay");
                read_ssid(reply, ssid);
                sd_bus_message_exit_container(reply);
            } else {
                sd_bus_message_skip(reply, "v");
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    if (!strcmp(type, "802-11-wireless") && ssid[0]) {
        snprintf(saved[saved_count].ssid, SSID_MAX, "%s", ssid);
        snprintf(saved[saved_count++].path, sizeof(saved[0].path), "%s", path);
    }
    sd_bus_message_unref(reply);
}

static const char *saved_path(const char *ssid) {
    for (int i = 0; i < saved_count; i++) {
        if (!strcmp(saved[i].ssid, ssid))
            return saved[i].path;
    }
    return NULL;
}

static char active_ap[128];

// A network nearby: each name once, as strong as its strongest access point.
static void add_access_point(const char *path) {
    sd_bus_message *reply = NULL;
    char ssid[SSID_MAX];
    struct wifi *w = NULL;
    uint8_t strength = 0;

    if (sd_bus_get_property(bus, NM, path, ACCESS_POINT, "Ssid", NULL, &reply, "ay") < 0)
        return;
    read_ssid(reply, ssid);
    sd_bus_message_unref(reply);
    if (!ssid[0]) // a hidden one, which says no name
        return;
    sd_bus_get_property_trivial(bus, NM, path, ACCESS_POINT, "Strength", NULL, 'y', &strength);
    for (int i = 0; i < now.wifi_count && !w; i++) {
        if (!strcmp(now.wifi[i].ssid, ssid))
            w = &now.wifi[i];
    }
    if (!w && now.wifi_count == WIFI_MAX)
        return;
    if (!w) {
        w = &now.wifi[now.wifi_count++];
        *w = (struct wifi){.strength = -1};
        snprintf(w->ssid, SSID_MAX, "%s", ssid);
    }
    w->strength = strength > w->strength ? strength : w->strength;
    w->secured |= (get_u32(path, ACCESS_POINT, "Flags") & AP_PRIVACY) || get_u32(path, ACCESS_POINT, "WpaFlags")
               || get_u32(path, ACCESS_POINT, "RsnFlags");
    w->active |= !strcmp(path, active_ap);
    w->saved = saved_path(ssid) != NULL;
}

// The joined network first, then the strongest.
static int compare_wifi(const void *a, const void *b) {
    const struct wifi *x = a, *y = b;

    return x->active != y->active ? y->active - x->active : y->strength - x->strength;
}

// The first address of an IP4Config, aa{sv}, its gateway, and its first DNS server.
static uint32_t read_address(sd_bus_message *m, uint32_t *netmask) {
    const char *key, *text = NULL;
    uint32_t prefix = 32, address = 0;
    struct in_addr in;

    if (sd_bus_message_enter_container(m, 'a', "a{sv}") <= 0 || sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
        return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        sd_bus_message_read(m, "s", &key);
        if (!strcmp(key, "address"))
            sd_bus_message_read(m, "v", "s", &text);
        else if (!strcmp(key, "prefix"))
            sd_bus_message_read(m, "v", "u", &prefix);
        else
            sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    if (text && inet_aton(text, &in))
        address = ntohl(in.s_addr);
    if (netmask)
        *netmask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
    return address;
}

static uint64_t statistic(const char *card, const char *name) {
    char path[300];
    unsigned long long n = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", card, name);
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%llu", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

// The address of whichever device's connected, the cable first.
static void read_addresses(const char *device) {
    sd_bus_message *reply = NULL;
    char config[128], card[32], gateway[64];
    struct in_addr in;

    get_string(device, DEVICE, "Ip4Config", 'o', config, sizeof(config));
    get_string(device, DEVICE, "Interface", 's', card, sizeof(card));
    if (!config[0] || !strcmp(config, "/"))
        return;
    if (sd_bus_get_property(bus, NM, config, IP4, "AddressData", NULL, &reply, "aa{sv}") >= 0)
        now.address = read_address(reply, &now.netmask);
    sd_bus_message_unref(reply);
    reply = NULL;
    if (sd_bus_get_property(bus, NM, config, IP4, "NameserverData", NULL, &reply, "aa{sv}") >= 0)
        now.dns = read_address(reply, NULL);
    sd_bus_message_unref(reply);
    get_string(config, IP4, "Gateway", 's', gateway, sizeof(gateway));
    if (inet_aton(gateway, &in))
        now.gateway = ntohl(in.s_addr);
    now.rx_bytes = statistic(card, "rx_bytes");
    now.tx_bytes = statistic(card, "tx_bytes");
}

static void refresh(void) {
    char joining[SSID_MAX];
    uint32_t reason[2] = {0};
    sd_bus_message *reply = NULL;

    snprintf(joining, sizeof(joining), "%s", now.joining);
    now = (struct network){0};
    snprintf(now.joining, sizeof(now.joining), "%s", joining);
    wifi_device[0] = wired_device[0] = active_ap[0] = '\0';
    saved_count = 0;
    stale = false;
    if (!bus || !(now.running = sd_bus_call_method(bus, NM, NM_PATH, NM, "GetDevices", NULL, NULL, "") >= 0))
        return;
    now.wifi_on = get_bool(NM_PATH, NM, "WirelessEnabled");
    for_each_path(NM_PATH, NM, "GetDevices", add_device);
    for_each_path(SETTINGS_PATH, SETTINGS, "ListConnections", add_saved);
    if (wired_device[0]) {
        now.cable = true;
        now.wired = get_u32(wired_device, DEVICE, "State") == STATE_ACTIVATED;
        get_string(wired_device, DEVICE, "Interface", 's', now.wired_name, sizeof(now.wired_name));
    }
    if (wifi_device[0]) {
        now.wifi_card = true;
        wifi_state = get_u32(wifi_device, DEVICE, "State");
        if (sd_bus_get_property(bus, NM, wifi_device, DEVICE, "StateReason", NULL, &reply, "(uu)") >= 0)
            sd_bus_message_read(reply, "(uu)", &reason[0], &reason[1]);
        sd_bus_message_unref(reply);
        wifi_reason = reason[1];
        get_string(wifi_device, WIRELESS, "ActiveAccessPoint", 'o', active_ap, sizeof(active_ap));
        for_each_path(wifi_device, WIRELESS, "GetAllAccessPoints", add_access_point);
        qsort(now.wifi, now.wifi_count, sizeof(now.wifi[0]), compare_wifi);
    }
    if (now.wired)
        read_addresses(wired_device);
    else if (wifi_device[0] && wifi_state == STATE_ACTIVATED)
        read_addresses(wifi_device);
}

// What joining's come to, after a refresh: 1 joined, -1 couldn't, 0 not yet.
static int join_done(void) {
    bool joined = now.wifi_count && now.wifi[0].active && !strcmp(now.wifi[0].ssid, now.joining);

    if (!now.joining[0])
        return 0;
    if (wifi_state == STATE_ACTIVATED && joined)
        return 1;
    if (wifi_state > STATE_DISCONNECTED && wifi_state < STATE_ACTIVATED)
        join_progressed = true;
    if (!(join_progressed && (wifi_state == STATE_FAILED || wifi_state == STATE_DISCONNECTED))
        && now_ms() - join_started < JOIN_MS)
        return 0;

    // A network made just now that couldn't be joined goes again, so it isn't saved with a wrong password.
    int reason = fail_reason ? fail_reason : wifi_reason;
    bool password = reason >= REASON_NO_SECRETS && reason <= REASON_SUPPLICANT_TIMEOUT;

    snprintf(error, sizeof(error), password ? "That password isn't right." : "Couldn't join %s.", now.joining);
    if (join_made[0])
        sd_bus_call_method(bus, NM, join_made, CONNECTION, "Delete", NULL, NULL, "");
    return -1;
}

void network_poll(struct pollfd *fd, int *timeout) {
    uint64_t usec = UINT64_MAX;

    *fd = (struct pollfd){.fd = bus ? sd_bus_get_fd(bus) : -1, .events = bus ? sd_bus_get_events(bus) : 0};
    if (bus && sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
        struct timespec t;
        int64_t left;

        clock_gettime(CLOCK_MONOTONIC, &t);
        left = (int64_t)(usec / 1000) - ((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
        *timeout = left > 0 ? (int)left : 0;
    } else {
        *timeout = -1;
    }
    if (stale) {
        int64_t wait = changed_at + REFRESH_MS - now_ms();

        wait = wait > 0 ? wait : 0;
        *timeout = *timeout < 0 || wait < *timeout ? (int)wait : *timeout;
    }
    if (now.joining[0]) // checked every second while it lasts
        *timeout = *timeout < 0 || *timeout > 1000 ? 1000 : *timeout;
}

bool network_read(int *done) {
    *done = 0;
    while (bus && sd_bus_process(bus, NULL) > 0)
        ;
    if (now.joining[0] && !stale && now_ms() - changed_at >= 1000) {
        stale = true;
        changed_at = now_ms() - REFRESH_MS;
    }
    if (!stale || now_ms() - changed_at < REFRESH_MS)
        return false;
    refresh();
    if ((*done = join_done()) != 0) {
        now.joining[0] = '\0';
        join_made[0] = '\0';
    }
    return true;
}

const struct network *network_now(void) {
    return &now;
}

const char *network_error(void) {
    return error;
}

static bool failed(sd_bus_error *e, const char *what) {
    snprintf(error, sizeof(error), "Couldn't %s: %s", what, e->message ? e->message : "NetworkManager said no");
    sd_bus_error_free(e);
    stale = true;
    return false;
}

void network_scan(void) {
    if (bus && wifi_device[0])
        sd_bus_call_method(bus, NM, wifi_device, WIRELESS, "RequestScan", NULL, NULL, "a{sv}", 0);
}

static void append_bytes(sd_bus_message *m, const char *key, const char *bytes) {
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "ay");
    sd_bus_message_append_array(m, 'y', bytes, strlen(bytes));
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

// A new connection, for whoever's logged in, as a{sa{sv}}: WPA2, or WPA3 where that's
// all the network has, with the password.
static void append_settings(sd_bus_message *m, const char *ssid, const char *password, bool hidden, bool sae) {
    struct passwd *me = getpwuid(getuid());
    char owner[96];

    snprintf(owner, sizeof(owner), "user:%s", me ? me->pw_name : "");
    sd_bus_message_open_container(m, 'a', "{sa{sv}}");
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", "connection");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_append(m, "{sv}", "id", "s", ssid);
    sd_bus_message_append(m, "{sv}", "type", "s", "802-11-wireless");
    sd_bus_message_append(m, "{sv}", "permissions", "as", 1, owner);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", "802-11-wireless");
    sd_bus_message_open_container(m, 'a', "{sv}");
    append_bytes(m, "ssid", ssid);
    sd_bus_message_append(m, "{sv}", "mode", "s", "infrastructure");
    if (hidden)
        sd_bus_message_append(m, "{sv}", "hidden", "b", 1);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    if (password && password[0]) {
        sd_bus_message_open_container(m, 'e', "sa{sv}");
        sd_bus_message_append(m, "s", "802-11-wireless-security");
        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_append(m, "{sv}", "key-mgmt", "s", sae ? "sae" : "wpa-psk");
        sd_bus_message_append(m, "{sv}", "psk", "s", password);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
    }
    sd_bus_message_close_container(m);
}

// The access point for a network, and whether WPA3 is all it takes.
static void find_access_point(const char *ssid, char *path, size_t size, bool *sae) {
    sd_bus_message *reply = NULL;
    const char *item;
    char name[SSID_MAX];

    snprintf(path, size, "/");
    *sae = false;
    if (sd_bus_call_method(bus, NM, wifi_device, WIRELESS, "GetAllAccessPoints", NULL, &reply, "") < 0)
        return;
    sd_bus_message_enter_container(reply, 'a', "o");
    while (sd_bus_message_read(reply, "o", &item) > 0) {
        sd_bus_message *ssid_reply = NULL;

        if (sd_bus_get_property(bus, NM, item, ACCESS_POINT, "Ssid", NULL, &ssid_reply, "ay") >= 0) {
            read_ssid(ssid_reply, name);
            if (!strcmp(name, ssid)) {
                uint32_t rsn = get_u32(item, ACCESS_POINT, "RsnFlags");

                snprintf(path, size, "%s", item);
                *sae = (rsn & KEY_MGMT_SAE) && !get_u32(item, ACCESS_POINT, "WpaFlags") && !(rsn & 0x100);
            }
        }
        sd_bus_message_unref(ssid_reply);
    }
    sd_bus_message_unref(reply);
}

bool network_join(const char *ssid, const char *password, bool hidden) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL, *reply = NULL;
    const char *saved = saved_path(ssid), *made = NULL, *active;
    char ap[128];
    bool sae;
    int r;

    error[0] = '\0';
    if (!bus || !wifi_device[0])
        return snprintf(error, sizeof(error), "There's no Wi-Fi card."), false;
    find_access_point(ssid, ap, sizeof(ap), &sae);
    if (saved && !(password && password[0])) {
        if (sd_bus_call_method(bus, NM, NM_PATH, NM, "ActivateConnection", &e, NULL, "ooo", saved, wifi_device, ap) < 0)
            return failed(&e, "join it");
        join_made[0] = '\0';
    } else {
        if (saved) // saved with another password, which this one replaces
            sd_bus_call_method(bus, NM, saved, CONNECTION, "Delete", NULL, NULL, "");
        sd_bus_message_new_method_call(bus, &m, NM, NM_PATH, NM, "AddAndActivateConnection");
        append_settings(m, ssid, password, hidden, sae);
        sd_bus_message_append(m, "oo", wifi_device, hidden ? "/" : ap);
        r = sd_bus_call(bus, m, 0, &e, &reply);
        sd_bus_message_unref(m);
        if (r < 0)
            return failed(&e, "join it");
        sd_bus_message_read(reply, "oo", &made, &active);
        snprintf(join_made, sizeof(join_made), "%s", made ? made : "");
        sd_bus_message_unref(reply);
    }
    snprintf(now.joining, sizeof(now.joining), "%s", ssid);
    join_started = now_ms();
    join_progressed = false;
    fail_reason = 0;
    stale = true;
    changed_at = now_ms();
    return true;
}

bool network_leave(void) {
    sd_bus_error e = SD_BUS_ERROR_NULL;

    if (!bus || !wifi_device[0])
        return false;
    if (sd_bus_call_method(bus, NM, wifi_device, DEVICE, "Disconnect", &e, NULL, "") < 0)
        return failed(&e, "leave it");
    stale = true;
    return true;
}

bool network_forget(const char *ssid) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    const char *path = saved_path(ssid);

    if (!bus || !path)
        return false;
    if (sd_bus_call_method(bus, NM, path, CONNECTION, "Delete", &e, NULL, "") < 0)
        return failed(&e, "forget it");
    stale = true;
    return true;
}

bool network_set_wifi(bool on) {
    sd_bus_error e = SD_BUS_ERROR_NULL;

    if (!bus)
        return false;
    if (sd_bus_set_property(bus, NM, NM_PATH, NM, "WirelessEnabled", &e, "b", on) < 0)
        return failed(&e, on ? "turn Wi-Fi on" : "turn Wi-Fi off");
    stale = true;
    return true;
}
