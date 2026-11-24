#include <aximos.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NETD_SOCKET "/run/netd.sock"

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

int net_ask(const char *line, char *reply, size_t size) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un at = {.sun_family = AF_UNIX, .sun_path = NETD_SOCKET};
    size_t have = 0;
    ssize_t n = 0;

    reply[0] = '\0';
    if (fd < 0 || connect(fd, (struct sockaddr *)&at, sizeof(at)) < 0 || dprintf(fd, "%s\n", line) < 0) {
        if (fd >= 0)
            close(fd);
        snprintf(reply, size, "error netd isn't running\n");
        return -1;
    }
    while (have + 1 < size && (n = read(fd, reply + have, size - 1 - have)) > 0)
        have += n;
    reply[have] = '\0';
    close(fd);
    return strncmp(reply, "error", 5) ? 0 : -1;
}

int net_info(struct net_info *out) {
    char reply[4096], card[32] = "", method[8] = "";

    *out = (struct net_info){0};
    if (net_ask("status", reply, sizeof(reply)) < 0)
        return 0; // no network, rather than a failure
    for (char *line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
        char a[32], b[64], c[16], d[16], e[16];
        struct in_addr in;

        if (!strcmp(line, "airplane on")) {
            out->airplane = true;
        } else if (sscanf(line, "card %31s %15s %15s %15s", a, c, d, e) == 4) {
            out->has_device = true;
            if (!card[0] && !strcmp(d, "up"))
                snprintf(method, sizeof(method), "%s", e);
        } else if (sscanf(line, "address %31s %63s", a, b) == 2 && !card[0] && strchr(b, '.')) {
            int prefix = atoi(strchr(b, '/') ? strchr(b, '/') + 1 : "32");

            *strchr(b, '/') = '\0';
            if (inet_aton(b, &in)) {
                snprintf(card, sizeof(card), "%s", a);
                out->address = ntohl(in.s_addr);
                out->netmask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
                out->automatic = strcmp(method, "hand") != 0;
            }
        } else if (sscanf(line, "gateway %31s %63s", a, b) == 2 && !strcmp(a, card) && inet_aton(b, &in)) {
            out->gateway = ntohl(in.s_addr);
        } else if (sscanf(line, "dns %63s", b) == 1 && !out->dns && inet_aton(b, &in)) {
            out->dns = ntohl(in.s_addr);
        }
    }
    if (card[0]) {
        out->rx_bytes = statistic(card, "rx_bytes");
        out->tx_bytes = statistic(card, "tx_bytes");
    }
    return 0;
}

int net_airplane(bool on) {
    char reply[256];

    return net_ask(on ? "airplane on" : "airplane off", reply, sizeof(reply));
}
