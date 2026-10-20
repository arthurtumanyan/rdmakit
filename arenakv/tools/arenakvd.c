/*
 * arenakvd.c - key-value cache server daemon.
 *
 * Usage: arenakvd [options]
 *   --transport tcp|rdma|auto   transport (default: auto = RDMA when a device exists, else TCP)
 *   --wait poll|event           waiting strategy of RDMA connections (default: event; poll spins one core per
 *                               connection thread and only pays off with one core per connection)
 *   --addr ADDRESS              listen address (default: all IPv4 addresses)
 *   --port PORT                 listen port (default: 7600)
 *   --capacity SIZE             cache memory, with optional K, M or G suffix (default: 256M)
 *   --shards N                  shards, power of two (default: 64)
 *   --avg-entry BYTES           expected mean entry size, sizes the hash index (default: 256)
 *   --max-msg BYTES             largest message, bounds key + value + 12 (default: 65536)
 *   --depth N                   receive and send buffers per connection (default: 16)
 *   --huge-pages                advise transparent huge pages for the cache memory
 *   --stats SECONDS             print a statistics line every SECONDS seconds
 * Stops on SIGINT or SIGTERM.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akvd.h"

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

/* Parses a size with optional K/M/G suffix; returns 0 on error. */
static uint64_t parse_size(const char *s) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0) {
        return 0;
    }
    switch (*end) {
    case '\0':
        break;
    case 'k':
    case 'K':
        v *= 1024.0;
        break;
    case 'm':
    case 'M':
        v *= 1024.0 * 1024.0;
        break;
    case 'g':
    case 'G':
        v *= 1024.0 * 1024.0 * 1024.0;
        break;
    default:
        return 0;
    }
    return (uint64_t)v;
}

int main(int argc, char **argv) {
    akvd_config cfg;
    akvd_config_default(&cfg);
    cfg.wait = DUCT_WAIT_EVENT;
    unsigned stats_every = 0;
    static const struct option lo[] = {
        {"transport", required_argument, NULL, 't'}, {"wait", required_argument, NULL, 'w'},
        {"addr", required_argument, NULL, 'a'},      {"port", required_argument, NULL, 'p'},
        {"capacity", required_argument, NULL, 'c'},  {"shards", required_argument, NULL, 's'},
        {"max-msg", required_argument, NULL, 'm'},   {"avg-entry", required_argument, NULL, 'e'},
        {"depth", required_argument, NULL, 'd'},     {"huge-pages", no_argument, NULL, 'H'},
        {"stats", required_argument, NULL, 'S'},     {NULL, 0, NULL, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 't':
            cfg.transport = strcmp(optarg, "tcp") == 0    ? DUCT_TRANSPORT_TCP
                            : strcmp(optarg, "rdma") == 0 ? DUCT_TRANSPORT_RDMA
                                                          : DUCT_TRANSPORT_AUTO;
            break;
        case 'w':
            cfg.wait = strcmp(optarg, "poll") == 0 ? DUCT_WAIT_POLL : DUCT_WAIT_EVENT;
            break;
        case 'a':
            cfg.addr = optarg;
            break;
        case 'p':
            cfg.port = optarg;
            break;
        case 'c':
            cfg.store.capacity = parse_size(optarg);
            break;
        case 's':
            cfg.store.shards = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'e':
            cfg.store.avg_entry = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'm':
            cfg.max_msg = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            cfg.recv_depth = cfg.send_depth = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'H':
            cfg.store.huge_pages = 1;
            break;
        case 'S':
            stats_every = (unsigned)strtoul(optarg, NULL, 10);
            break;
        default:
            return 2;
        }
    }
    if (cfg.store.capacity == 0) {
        fprintf(stderr, "invalid capacity\n");
        return 2;
    }
    cfg.store.max_entry = cfg.max_msg;
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int err = 0;
    akvd *d = akvd_start(&cfg, &err);
    if (d == NULL) {
        fprintf(stderr, "cannot start server: %s\n", duct_strerror(err));
        return 1;
    }
    printf("arenakvd: listening on port %d, %llu MiB in %u shards, tag matching: %s\n", akvd_port(d),
           (unsigned long long)(cfg.store.capacity >> 20), cfg.store.shards ? cfg.store.shards : 64,
           akv_simd_name());
    fflush(stdout);
    unsigned elapsed = 0;
    while (!stop_requested) {
        sleep(1);
        if (stats_every != 0 && ++elapsed % stats_every == 0) {
            akv_stats st;
            akv_stats_get(akvd_store(d), &st);
            printf("entries=%llu log=%llu/%llu MiB gets=%llu hits=%llu puts=%llu evictions=%llu "
                   "reinsertions=%llu conns=%u\n",
                   (unsigned long long)st.entries, (unsigned long long)(st.log_bytes >> 20),
                   (unsigned long long)(st.arena_bytes >> 20), (unsigned long long)st.gets,
                   (unsigned long long)st.hits, (unsigned long long)st.puts, (unsigned long long)st.evictions,
                   (unsigned long long)st.reinsertions, akvd_connections(d));
            fflush(stdout);
        }
    }
    akvd_stop(d);
    return 0;
}
