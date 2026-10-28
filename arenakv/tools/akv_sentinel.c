/*
 * akv_sentinel.c - automatic failover monitor for a replicated arenakvd group (src/sentinel.h).
 *
 * Run one monitor per host (at least three, on hosts that fail independently), each listing every node
 * of the group and the other monitors:
 *
 *   akv_sentinel --name cache --listen 26379 --state /var/lib/arenakv/sentinel.state \
 *       --node 10.0.0.1:6379:7600 --node 10.0.0.2:6379:7600 --node 10.0.0.3:6379:7600 \
 *       --peer 10.0.0.5:26379 --peer 10.0.0.6:26379 [--quorum 2] [--down-after 5000] \
 *       [--failover-timeout 60000] [--node-auth USER:PASSWORD]
 *
 * The first --node is the primary until a failover; the state file keeps the current one. Events are
 * written to standard error. SIGINT and SIGTERM stop the monitor.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sentinel.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static int usage(void) {
    fprintf(
        stderr,
        "usage: akv_sentinel --listen [ADDR:]PORT --node HOST:RESP_PORT:NATIVE_PORT --node ... \\\n"
        "                    [--peer HOST:PORT ...] [--name NAME] [--state FILE] [--quorum N]\\\n"
        "                    [--down-after MS] [--failover-timeout MS] [--node-auth [USER:]PASSWORD]\n"
        "  --listen           address of this monitor (clients and the other monitors connect here)\n"
        "  --node             a member of the group: address, RESP port, native port; the first is the\n"
        "                     primary until a failover (every monitor lists the same nodes in the same "
        "order)\n"
        "  --peer             another monitor of the group (repeat for each)\n"
        "  --name             group name used by clients (default: arenakv)\n"
        "  --state            file that keeps epochs, the vote and the current primary across restarts\n"
        "  --quorum           monitors that must see the primary down (default: a majority)\n"
        "  --down-after       milliseconds without a valid reply before the primary counts as down "
        "(default: 5000)\n"
        "  --failover-timeout milliseconds for a promotion, and between election attempts (default: 60000)\n"
        "  --node-auth        credentials for the nodes; the user needs the admin category (REPLICAOF)\n");
    return 2;
}

/* Parses an unsigned number in [lo, hi]. Returns 0 or -1. */
static int num(const char *s, unsigned long lo, unsigned long hi, unsigned *out) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = (unsigned)v;
    return 0;
}

/* Splits "HOST:PORT" at the last colon. Returns 0 or -1. */
static int hostport(const char *s, char *host, size_t hcap, unsigned *port) {
    const char *c = strrchr(s, ':');
    if (c == NULL || (size_t)(c - s) >= hcap || c == s || num(c + 1, 1, 65535, port) != 0) {
        return -1;
    }
    memcpy(host, s, (size_t)(c - s));
    host[c - s] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    sentinel_cfg cfg;
    sentinel_cfg_default(&cfg);
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (v == NULL) {
            return usage();
        }
        unsigned port;
        if (strcmp(a, "--listen") == 0) {
            const char *c = strrchr(v, ':');
            if (c == NULL) {
                snprintf(cfg.listen_port, sizeof(cfg.listen_port), "%s", v);
            } else if ((size_t)(c - v) < sizeof(cfg.listen_addr)) {
                memcpy(cfg.listen_addr, v, (size_t)(c - v));
                cfg.listen_addr[c - v] = '\0';
                snprintf(cfg.listen_port, sizeof(cfg.listen_port), "%s", c + 1);
            }
            if (num(cfg.listen_port, 0, 65535, &port) != 0) {
                fprintf(stderr, "invalid --listen %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--node") == 0) {
            /* HOST:RESP:NATIVE: the native port follows the last colon, the RESP port the one before */
            char hr[300];
            unsigned native;
            const char *c = strrchr(v, ':');
            if (cfg.nnodes == SENTINEL_MAX_NODES || c == NULL || (size_t)(c - v) >= sizeof(hr) ||
                num(c + 1, 1, 65535, &native) != 0) {
                fprintf(stderr, "invalid --node %s (HOST:RESP_PORT:NATIVE_PORT)\n", v);
                return 2;
            }
            memcpy(hr, v, (size_t)(c - v));
            hr[c - v] = '\0';
            sentinel_node_cfg *n = &cfg.nodes[cfg.nnodes];
            if (hostport(hr, n->host, sizeof(n->host), &port) != 0) {
                fprintf(stderr, "invalid --node %s (HOST:RESP_PORT:NATIVE_PORT)\n", v);
                return 2;
            }
            n->resp_port = (int)port;
            n->native_port = (int)native;
            cfg.nnodes++;
        } else if (strcmp(a, "--peer") == 0) {
            sentinel_peer_cfg *p = &cfg.peers[cfg.npeers];
            if (cfg.npeers == SENTINEL_MAX_PEERS || hostport(v, p->host, sizeof(p->host), &port) != 0) {
                fprintf(stderr, "invalid --peer %s (HOST:PORT)\n", v);
                return 2;
            }
            p->port = (int)port;
            cfg.npeers++;
        } else if (strcmp(a, "--name") == 0) {
            if (strlen(v) == 0 || strlen(v) >= sizeof(cfg.name)) {
                fprintf(stderr, "invalid --name\n");
                return 2;
            }
            snprintf(cfg.name, sizeof(cfg.name), "%s", v);
        } else if (strcmp(a, "--state") == 0) {
            snprintf(cfg.state_path, sizeof(cfg.state_path), "%s", v);
        } else if (strcmp(a, "--quorum") == 0) {
            if (num(v, 1, SENTINEL_MAX_PEERS + 1, &cfg.quorum) != 0) {
                fprintf(stderr, "invalid --quorum %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--down-after") == 0) {
            if (num(v, 100, 3600000, &cfg.down_after_ms) != 0) {
                fprintf(stderr, "invalid --down-after %s (100 to 3600000 ms)\n", v);
                return 2;
            }
        } else if (strcmp(a, "--failover-timeout") == 0) {
            if (num(v, 1000, 86400000, &cfg.failover_timeout_ms) != 0) {
                fprintf(stderr, "invalid --failover-timeout %s (1000 to 86400000 ms)\n", v);
                return 2;
            }
        } else if (strcmp(a, "--node-auth") == 0) {
            const char *c = strchr(v, ':');
            if (c == NULL) {
                snprintf(cfg.node_pass, sizeof(cfg.node_pass), "%s", v);
            } else {
                snprintf(cfg.node_user, sizeof(cfg.node_user), "%.*s", (int)(c - v), v);
                snprintf(cfg.node_pass, sizeof(cfg.node_pass), "%s", c + 1);
            }
        } else {
            return usage();
        }
    }
    if (cfg.nnodes < 2) {
        fprintf(stderr, "at least two --node are needed (a primary and a replica)\n");
        return 2;
    }
    if (cfg.state_path[0] == '\0') {
        fprintf(stderr,
                "warning: no --state file: a restarted monitor forgets its vote and the current primary\n");
    }
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    char err[512];
    sentinel *s = sentinel_start(&cfg, err, sizeof(err));
    if (s == NULL) {
        fprintf(stderr, "akv_sentinel: %s\n", err);
        return 1;
    }
    while (!stop) {
        pause();
    }
    sentinel_stop(s);
    return 0;
}
