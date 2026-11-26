#include "bluetooth.h"
#include <fcntl.h>
#include <linux/rfkill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

// BlueZ, over D-Bus with sd-bus, on wm's own poll loop. Any signal it sends marks
// what's known as stale, and it's all asked for again a little later, as a scan
// sends many. Pairing and connecting are asynchronous calls, so wm doesn't wait.

#define BLUEZ "org.bluez"
#define ADAPTER BLUEZ ".Adapter1"
#define DEVICE BLUEZ ".Device1"
#define BATTERY BLUEZ ".Battery1"
#define AGENT_PATH "/org/aximos/BluetoothAgent"
#define REJECTED BLUEZ ".Error.Rejected"
#define REFRESH_MS 300
#define PAIR_USEC (90 * 1000000ULL) // pairing waits for whoever's confirming
#define CONNECT_USEC (30 * 1000000ULL)

static sd_bus *bus;
static struct bluetooth now;
static struct bt_request request;
static sd_bus_message *asked; // the agent call being answered
static bool stale = true, registered;
static int64_t changed_at;
static int result; // what the last call came to, for bluetooth_read
static char adapter[64], error[128];

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void mark_stale(void) {
    if (!stale)
        changed_at = now_ms();
    stale = true;
}

static int on_signal(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)m, (void)data, (void)e;
    mark_stale();
    return 0;
}

static int on_owner(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *name;

    (void)data, (void)e;
    if (sd_bus_message_read(m, "s", &name) >= 0 && !strcmp(name, BLUEZ)) {
        registered = false; // a new bluetoothd has no agent
        mark_stale();
    }
    return 0;
}

static const struct bt_device *device_at(const char *path) {
    for (int i = 0; i < now.count; i++) {
        if (!strcmp(now.devices[i].path, path))
            return &now.devices[i];
    }
    return NULL;
}

static const char *name_of(const char *path) {
    const struct bt_device *d = device_at(path);

    return d ? d->name : "the device";
}

// The agent: what pairing asks of whoever's at the computer.

static void forget_asked(void) {
    sd_bus_message_unref(asked);
    asked = NULL;
    request = (struct bt_request){0};
}

static void ask(sd_bus_message *m, enum bt_asking asking, const char *path, const char *code) {
    if (asked)
        sd_bus_reply_method_errorf(asked, REJECTED, "Asked something else");
    forget_asked();
    request.asking = asking;
    snprintf(request.device, sizeof(request.device), "%s", name_of(path));
    snprintf(request.code, sizeof(request.code), "%s", code ? code : "");
    if (asking != BT_SHOW_CODE)
        asked = sd_bus_message_ref(m);
    printf("wm: bluetooth asks %s for %s%s%s\n", asking == BT_CONFIRM ? "a match" : asking == BT_AUTHORIZE ? "a yes" : "a code typed",
           request.device, code ? ", " : "", code ? code : "");
    fflush(stdout);
}

static int agent_release(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)data, (void)e;
    registered = false;
    return sd_bus_reply_method_return(m, "");
}

// A device of the old kind asking for a PIN, such as a keyboard, which is to be typed
// on it, so one's made up and shown.
static int agent_pin(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path;
    char pin[8];

    (void)data, (void)e;
    if (sd_bus_message_read(m, "o", &path) < 0)
        return sd_bus_reply_method_errorf(m, REJECTED, "No device");
    snprintf(pin, sizeof(pin), "%06u", (unsigned)(arc4random() % 1000000));
    ask(m, BT_SHOW_CODE, path, pin);
    return sd_bus_reply_method_return(m, "s", pin);
}

static int agent_show_pin(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path, *pin;

    (void)data, (void)e;
    if (sd_bus_message_read(m, "os", &path, &pin) >= 0)
        ask(m, BT_SHOW_CODE, path, pin);
    return sd_bus_reply_method_return(m, "");
}

static int agent_show_passkey(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path;
    uint32_t key;
    uint16_t typed;
    char code[8];

    (void)data, (void)e;
    if (sd_bus_message_read(m, "ouq", &path, &key, &typed) >= 0 && request.asking != BT_SHOW_CODE) {
        snprintf(code, sizeof(code), "%06u", key % 1000000);
        ask(m, BT_SHOW_CODE, path, code);
    }
    return sd_bus_reply_method_return(m, "");
}

static int agent_passkey(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)data, (void)e;
    return sd_bus_reply_method_errorf(m, REJECTED, "There's no keyboard to type it with here");
}

static int agent_confirm(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path;
    uint32_t key;
    char code[8];

    (void)data, (void)e;
    if (sd_bus_message_read(m, "ou", &path, &key) < 0)
        return sd_bus_reply_method_errorf(m, REJECTED, "No device");
    snprintf(code, sizeof(code), "%06u", key % 1000000);
    ask(m, BT_CONFIRM, path, code);
    return 1; // answered by bluetooth_answer
}

static int agent_authorize(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path;

    (void)data, (void)e;
    if (sd_bus_message_read(m, "o", &path) < 0)
        return sd_bus_reply_method_errorf(m, REJECTED, "No device");
    ask(m, BT_AUTHORIZE, path, NULL);
    return 1;
}

// A device using one of the computer's services, such as its sound, which only one
// that's been paired may.
static int agent_service(sd_bus_message *m, void *data, sd_bus_error *e) {
    const char *path;
    int paired = 0;

    (void)data, (void)e;
    if (sd_bus_message_read(m, "o", &path) < 0
        || sd_bus_get_property_trivial(bus, BLUEZ, path, DEVICE, "Paired", NULL, 'b', &paired) < 0 || !paired)
        return sd_bus_reply_method_errorf(m, REJECTED, "Not paired");
    return sd_bus_reply_method_return(m, "");
}

static int agent_cancel(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)data, (void)e;
    if (request.asking)
        printf("wm: bluetooth stopped asking\n");
    forget_asked();
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_pin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_show_pin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_show_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_confirm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_authorize, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_service, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

void bluetooth_answer(bool yes) {
    if (asked) {
        if (yes)
            sd_bus_reply_method_return(asked, "");
        else
            sd_bus_reply_method_errorf(asked, REJECTED, "Said no");
        printf("wm: bluetooth answered %s\n", yes ? "yes" : "no");
    }
    forget_asked();
}

// Signals from BlueZ, and its coming and going, and the agent it calls.
bool bluetooth_open(void) {
    if (sd_bus_open_system(&bus) < 0) {
        bus = NULL;
        return false;
    }
    // By path, not by sender, as a signal's sender is BlueZ's unique name, not org.bluez:
    // its objects' properties, and objects coming and going.
    sd_bus_add_match(bus, NULL, "type='signal',path_namespace='/org/bluez'", on_signal, NULL);
    sd_bus_add_match(bus, NULL, "type='signal',interface='org.freedesktop.DBus.ObjectManager',arg0path='/org/bluez/'",
                     on_signal, NULL);
    sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner, NULL);
    sd_bus_add_object_vtable(bus, NULL, AGENT_PATH, BLUEZ ".Agent1", agent, NULL);
    return true;
}

static void register_agent(void) {
    // DisplayYesNo: pairing either just works, or shows a code to check or type.
    registered = sd_bus_call_method(bus, BLUEZ, "/org/bluez", BLUEZ ".AgentManager1", "RegisterAgent", NULL, NULL, "os",
                                    AGENT_PATH, "DisplayYesNo") >= 0;
    if (registered) {
        sd_bus_call_method(bus, BLUEZ, "/org/bluez", BLUEZ ".AgentManager1", "RequestDefaultAgent", NULL, NULL, "o",
                           AGENT_PATH);
        printf("wm: bluetooth's agent\n");
    }
}

// rfkill, as airplane mode and the keyboard's Bluetooth key have it.
static bool rfkill_blocked(void) {
    char path[64], text[32];
    bool blocked = false;

    for (int i = 0; i < 64; i++) {
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", i);
        if (!(f = fopen(path, "r")))
            continue;
        bool bt = fgets(text, sizeof(text), f) && !strncmp(text, "bluetooth", 9);

        fclose(f);
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/soft", i);
        if (bt && (f = fopen(path, "r"))) {
            blocked |= fgets(text, sizeof(text), f) && text[0] == '1';
            fclose(f);
        }
    }
    return blocked;
}

static bool rfkill_set(bool block) {
    struct rfkill_event e = {.type = RFKILL_TYPE_BLUETOOTH, .op = RFKILL_OP_CHANGE_ALL, .soft = block};
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    bool ok = fd >= 0 && write(fd, &e, sizeof(e)) == sizeof(e);

    if (fd >= 0)
        close(fd);
    return ok;
}

// A property's value, if it's of the type wanted, and skipped if not.
static bool variant(sd_bus_message *m, const char *type, void *out) {
    const char *contents;

    if (sd_bus_message_peek_type(m, NULL, &contents) < 0 || strcmp(contents, type)) {
        sd_bus_message_skip(m, "v");
        return false;
    }
    return sd_bus_message_read(m, "v", type, out) >= 0;
}

static void read_adapter(sd_bus_message *m) {
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key;
        int b;

        sd_bus_message_read(m, "s", &key);
        if (!strcmp(key, "Powered") && variant(m, "b", &b))
            now.powered = b;
        else if (!strcmp(key, "Discovering") && variant(m, "b", &b))
            now.discovering = b;
        else
            sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
}

// A device's properties, and its battery's, into d. Returns whether it has a name of
// its own, rather than just an address.
static bool read_device(sd_bus_message *m, struct bt_device *d) {
    bool named = false;

    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key, *s;
        int b;
        uint8_t percent;

        sd_bus_message_read(m, "s", &key);
        if (!strcmp(key, "Name") && variant(m, "s", &s))
            snprintf(d->name, sizeof(d->name), "%s", s), named = true;
        else if (!strcmp(key, "Alias") && variant(m, "s", &s)) {
            if (!named)
                snprintf(d->name, sizeof(d->name), "%s", s);
        } else if (!strcmp(key, "Address") && variant(m, "s", &s))
            snprintf(d->address, sizeof(d->address), "%s", s);
        else if (!strcmp(key, "Icon") && variant(m, "s", &s))
            snprintf(d->icon, sizeof(d->icon), "%s", s);
        else if (!strcmp(key, "Paired") && variant(m, "b", &b))
            d->paired = b;
        else if (!strcmp(key, "Connected") && variant(m, "b", &b))
            d->connected = b;
        else if (!strcmp(key, "RSSI"))
            variant(m, "n", &d->rssi);
        else if (!strcmp(key, "Percentage") && variant(m, "y", &percent))
            d->battery = percent;
        else
            sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    return named;
}

static int compare_devices(const void *a, const void *b) {
    const struct bt_device *x = a, *y = b;

    if (x->connected != y->connected)
        return y->connected - x->connected;
    if (x->paired != y->paired)
        return y->paired - x->paired;
    return y->rssi - x->rssi;
}

// Everything BlueZ has, at once: the first adapter, and its devices. Those nearby
// with only an address, as many are, aren't worth listing.
static void refresh(void) {
    sd_bus_message *reply = NULL;
    char busy[64], doing[16];

    snprintf(busy, sizeof(busy), "%s", now.busy);
    snprintf(doing, sizeof(doing), "%s", now.doing);
    now = (struct bluetooth){0};
    snprintf(now.busy, sizeof(now.busy), "%s", busy);
    snprintf(now.doing, sizeof(now.doing), "%s", doing);
    adapter[0] = '\0';
    stale = false;
    now.blocked = rfkill_blocked();
    if (!bus || sd_bus_call_method(bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
                                   &reply, "") < 0)
        return;
    now.running = true;
    if (!registered)
        register_agent();
    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char *path;
        struct bt_device d = {.battery = -1};
        bool device = false, named = false;

        sd_bus_message_read(reply, "o", &path);
        snprintf(d.path, sizeof(d.path), "%s", path);
        sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface;

            sd_bus_message_read(reply, "s", &iface);
            sd_bus_message_enter_container(reply, 'a', "{sv}");
            if (!strcmp(iface, ADAPTER) && !adapter[0]) {
                snprintf(adapter, sizeof(adapter), "%s", path);
                now.adapter = true;
                read_adapter(reply);
            } else if (!strcmp(iface, DEVICE) || !strcmp(iface, BATTERY)) {
                device |= !strcmp(iface, DEVICE);
                named |= read_device(reply, &d);
            } else {
                while (sd_bus_message_skip(reply, "{sv}") > 0)
                    ;
            }
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        if (device && now.count < BT_MAX && (named || d.paired || d.connected))
            now.devices[now.count++] = d;
    }
    sd_bus_message_unref(reply);

    // Only the first adapter's devices.
    int kept = 0;
    size_t length = strlen(adapter);

    for (int i = 0; i < now.count; i++) {
        if (length && !strncmp(now.devices[i].path, adapter, length) && now.devices[i].path[length] == '/')
            now.devices[kept++] = now.devices[i];
    }
    now.count = kept;
    qsort(now.devices, now.count, sizeof(now.devices[0]), compare_devices);
    if (now.busy[0] && !device_at(now.busy))
        now.busy[0] = now.doing[0] = '\0';
}

void bluetooth_poll(struct pollfd *fd, int *timeout) {
    uint64_t usec = UINT64_MAX;

    *fd = (struct pollfd){.fd = bus ? sd_bus_get_fd(bus) : -1, .events = bus ? sd_bus_get_events(bus) : 0};
    *timeout = -1;
    if (bus && sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
        struct timespec t;
        int64_t left;

        clock_gettime(CLOCK_MONOTONIC, &t);
        left = (int64_t)(usec / 1000) - ((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
        *timeout = left > 0 ? (int)left : 0;
    }
    if (stale) {
        int64_t wait = changed_at + REFRESH_MS - now_ms();

        wait = wait > 0 ? wait : 0;
        *timeout = *timeout < 0 || wait < *timeout ? (int)wait : *timeout;
    }
}

bool bluetooth_read(int *done) {
    struct bt_request was = request;

    *done = 0;
    while (bus && sd_bus_process(bus, NULL) > 0)
        ;
    *done = result;
    result = 0;
    if (*done)
        mark_stale();
    if (stale && now_ms() - changed_at >= REFRESH_MS) {
        refresh();
        return true;
    }
    return *done || memcmp(&was, &request, sizeof(request)) != 0;
}

const struct bluetooth *bluetooth_now(void) {
    return &now;
}

const struct bt_request *bluetooth_request(void) {
    return &request;
}

const char *bluetooth_error(void) {
    return error;
}

bool bluetooth_set_on(bool on) {
    bool done = rfkill_set(!on);

    // Without rfkill, as in a VM, or once it's unblocked, the adapter itself.
    if ((on || !done) && bus && adapter[0])
        done |= sd_bus_set_property(bus, BLUEZ, adapter, ADAPTER, "Powered", NULL, "b", on) >= 0;
    mark_stale();
    return done;
}

void bluetooth_look(bool on) {
    if (bus && adapter[0] && now.powered && on != now.discovering)
        sd_bus_call_method(bus, BLUEZ, adapter, ADAPTER, on ? "StartDiscovery" : "StopDiscovery", NULL, NULL, "");
}

// Asynchronous calls: pairing, then connecting, or disconnecting.

static int call_done(sd_bus_message *reply, void *data, sd_bus_error *e);

static bool start(const char *path, const char *method, const char *doing, uint64_t usec) {
    sd_bus_message *m = NULL;
    bool ok = bus && sd_bus_message_new_method_call(bus, &m, BLUEZ, path, DEVICE, method) >= 0
              && sd_bus_call_async(bus, NULL, m, call_done, (void *)method, usec) >= 0;

    sd_bus_message_unref(m);
    if (!ok) {
        snprintf(error, sizeof(error), "Couldn't reach Bluetooth.");
        return false;
    }
    snprintf(now.busy, sizeof(now.busy), "%s", path);
    snprintf(now.doing, sizeof(now.doing), "%s", doing);
    return true;
}

static int call_done(sd_bus_message *reply, void *data, sd_bus_error *e) {
    const char *method = data, *name = name_of(now.busy);
    const sd_bus_error *failed = sd_bus_message_get_error(reply);
    char path[64];

    (void)e;
    snprintf(path, sizeof(path), "%s", now.busy);
    now.busy[0] = now.doing[0] = '\0';
    if (failed && !(strcmp(method, "Pair") == 0 && sd_bus_error_has_name(failed, BLUEZ ".Error.AlreadyExists"))) {
        printf("wm: bluetooth couldn't %s %s: %s\n", method, name, failed->message ? failed->message : failed->name);
        snprintf(error, sizeof(error), !strcmp(method, "Pair") ? "Couldn't pair with %s." : !strcmp(method, "Connect")
                                       ? "Couldn't connect to %s." : "Couldn't disconnect %s.", name);
        if (request.asking == BT_SHOW_CODE)
            forget_asked();
        result = -1;
        return 0;
    }
    if (!strcmp(method, "Pair")) { // trusted, so it can connect again by itself, and then connected
        sd_bus_set_property(bus, BLUEZ, path, DEVICE, "Trusted", NULL, "b", 1);
        if (request.asking == BT_SHOW_CODE)
            forget_asked();
        printf("wm: bluetooth paired %s\n", name);
        if (!start(path, "Connect", "Connecting", CONNECT_USEC))
            result = -1;
        return 0;
    }
    printf("wm: bluetooth %s %s\n", !strcmp(method, "Connect") ? "connected" : "disconnected", name);
    error[0] = '\0';
    result = 1;
    return 0;
}

bool bluetooth_connect(const char *path) {
    const struct bt_device *d = device_at(path);

    if (!d || now.busy[0])
        return false;
    bluetooth_look(false); // looking slows connecting down
    return d->paired ? start(path, "Connect", "Connecting", CONNECT_USEC) : start(path, "Pair", "Pairing", PAIR_USEC);
}

bool bluetooth_disconnect(const char *path) {
    return device_at(path) && !now.busy[0] && start(path, "Disconnect", "Disconnecting", CONNECT_USEC);
}

bool bluetooth_forget(const char *path) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    char name[64];

    snprintf(name, sizeof(name), "%s", name_of(path));
    if (!bus || !adapter[0] || sd_bus_call_method(bus, BLUEZ, adapter, ADAPTER, "RemoveDevice", &e, NULL, "o", path) < 0) {
        snprintf(error, sizeof(error), "Couldn't forget %s.", name);
        sd_bus_error_free(&e);
        return false;
    }
    printf("wm: bluetooth forgot %s\n", name);
    mark_stale();
    return true;
}
