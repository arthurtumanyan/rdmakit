/*
 * test_drivers.c - the three drivers against an in-process arenakv server (needs the native driver build,
 * which links libarenakv).
 *
 * For each driver (resp, memcached, native):
 *   - synchronous SET, GET, DEL, INCR and MGET with expected results;
 *   - asynchronous: 400 pipelined requests of all types, among them large values (200 KiB, native 48 KiB)
 *     that arrive in many segments, so that the reply parsers see incomplete input; replies are checked
 *     in order;
 *   - authentication: a wrong secret is refused, the right one accepted (server with credentials);
 *   - the runner end to end in count mode with several connections per thread and pipelining: the
 *     expected number of operations and no errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "driver.h"
#include "runner.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

#ifdef PACE_HAVE_NATIVE
#include "akvd.h"

static const char *port_of(const pace_driver *d, akvd *s, char *buf) {
    int p = strcmp(d->name, "resp") == 0        ? akvd_resp_port(s)
            : strcmp(d->name, "memcached") == 0 ? akvd_memcached_port(s)
                                                : akvd_port(s);
    snprintf(buf, 16, "%d", p);
    return buf;
}

static void test_sync(const pace_driver *d, const char *port) {
    pace_conn *c = d->connect("127.0.0.1", port);
    EXPECT(c != NULL, "%s: connect", d->name);
    if (c == NULL) {
        return;
    }
    char buf[256];
    size_t n;
    EXPECT(d->put(c, "s1", 2, "v1", 2) == PACE_OK, "%s: put", d->name);
    EXPECT(d->get(c, "s1", 2, buf, sizeof(buf), &n) == PACE_OK && n == 2 && memcmp(buf, "v1", 2) == 0,
           "%s: get", d->name);
    EXPECT(d->get(c, "none", 4, buf, sizeof(buf), &n) == PACE_NOTFOUND, "%s: miss", d->name);
    char cnt[32];
    size_t cl = (size_t)snprintf(cnt, sizeof(cnt), "cnt-%s", d->name); /* drivers share the server */
    EXPECT(d->incr(c, cnt, cl) == PACE_OK && d->incr(c, cnt, cl) == PACE_OK,
           "%s: incr creates and increments", d->name);
    EXPECT(d->get(c, cnt, cl, buf, sizeof(buf), &n) == PACE_OK && n == 1 && buf[0] == '2',
           "%s: counter value", d->name);
    d->put(c, "s2", 2, "v2", 2);
    const char *keys[3] = {"s1", "none", "s2"};
    size_t kl[3] = {2, 4, 2};
    unsigned hits = 0;
    EXPECT(d->mget(c, keys, kl, 3, &hits) == PACE_OK && hits == 2, "%s: mget found %u of 3", d->name, hits);
    EXPECT(d->del(c, "s1", 2) == PACE_OK && d->del(c, "s1", 2) == PACE_NOTFOUND, "%s: del", d->name);
    d->close(c);
}

static void test_async(const pace_driver *d, const char *port) {
    const pace_adriver *ad = d->async;
    pace_aconn *c = ad->connect("127.0.0.1", port);
    EXPECT(c != NULL, "%s: async connect", d->name);
    if (c == NULL) {
        return;
    }
    /* Large values arrive in many segments; the native protocol is bounded by its 64 KiB messages. */
    size_t big = strcmp(d->name, "native") == 0 ? 48u << 10 : 200u << 10;
    char *val = malloc(big);
    memset(val, 'v', big);
    enum { N = 400 };
    int want[N];
    unsigned want_hits[N];
    unsigned window = ad->window(c);
    unsigned limit = window == 0 ? N : window;
    int sent = 0;
    int got = 0;
    int bad = 0;
    int epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 0};
    epoll_ctl(epfd, EPOLL_CTL_ADD, ad->fd(c), &ev);
    while (got < N && bad == 0) {
        while (sent < N && (unsigned)(sent - got) < limit) {
            char key[32];
            int kl = snprintf(key, sizeof(key), "a%d", sent / 5);
            pace_req r = {.key = key, .klen = (size_t)kl};
            switch (sent % 5) {
            case 0: /* SET, every 10th with a large value */
                r.type = PACE_OP_SET;
                r.val = val;
                r.vlen = sent % 50 == 0 ? big : 64;
                want[sent] = PACE_OK;
                break;
            case 1:
                r.type = PACE_OP_GET;
                want[sent] = PACE_OK;
                break;
            case 2:
                r.type = PACE_OP_INCR;
                r.key = "acnt";
                r.klen = 4;
                want[sent] = PACE_OK;
                break;
            case 3: {
                static const char *mk[2];
                static size_t mkl[2];
                static char k2[32];
                mk[0] = key;
                mkl[0] = (size_t)kl;
                mkl[1] = (size_t)snprintf(k2, sizeof(k2), "missing%d", sent);
                mk[1] = k2;
                r.type = PACE_OP_MGET;
                r.keys = mk;
                r.klens = mkl;
                r.nkeys = 2;
                want[sent] = PACE_OK;
                want_hits[sent] = 1;
                break;
            }
            default:
                r.type = PACE_OP_DEL;
                want[sent] = PACE_OK;
                break;
            }
            if (ad->submit(c, &r) != PACE_OK) {
                bad = 1;
                break;
            }
            sent++;
        }
        if (ad->io(c) != PACE_OK) {
            bad = 1;
            break;
        }
        pace_reply rep;
        int k;
        while ((k = ad->next(c, &rep)) == 1) {
            if (rep.status != want[got] || ((got % 5) == 3 && rep.hits != want_hits[got])) {
                EXPECT(0, "%s: reply %d status %d hits %u, expected %d", d->name, got, rep.status, rep.hits,
                       want[got]);
                bad = 1;
            }
            got++;
        }
        if (k < 0) {
            EXPECT(0, "%s: reply parser failed at %d", d->name, got);
            bad = 1;
        }
        if (got < N && ad->arm(c) == 0) {
            struct epoll_event e;
            epoll_wait(epfd, &e, 1, 2000);
        }
    }
    EXPECT(got == N && bad == 0, "%s: %d of %d pipelined replies", d->name, got, N);
    close(epfd);
    ad->close(c);
    free(val);
}

static void test_runner(const pace_driver *d, const char *port) {
    pace_config cfg;
    pace_config_default(&cfg);
    cfg.driver = d;
    cfg.port = port;
    cfg.threads = 2;
    cfg.conns_per_thread = 3;
    cfg.pipeline = 4;
    cfg.requests = 300;
    cfg.wl.nkeys = 500;
    pace_wl_parse_mix(&cfg.wl, "get=50,set=20,del=5,incr=5,mget=10,rmw=10");
    cfg.wl.mget_keys = 3;
    pace_wl_parse_vsize(&cfg.wl, "16-2048");
    static pace_result res;
    EXPECT(pace_run(&cfg, &res) == 0, "%s: runner", d->name);
    EXPECT(pace_result_ops(&res) == 2 * 3 * 300 && res.errors == 0, "%s: runner ops %llu errors %llu",
           d->name, (unsigned long long)pace_result_ops(&res), (unsigned long long)res.errors);
    EXPECT(res.mget_keys == 3 * res.ops_of[PACE_OP_MGET], "%s: mget keys", d->name);
}

int main(void) {
    char cred[] = "/tmp/pace-cred-XXXXXX";
    int fd = mkstemp(cred);
    if (write(fd, "tester:s3cret\n", 14) != 14) {
        EXPECT(0, "credentials");
    }
    close(fd);
    akvd_config sc;
    akvd_config_default(&sc);
    sc.transport = DUCT_TRANSPORT_TCP;
    sc.addr = "127.0.0.1";
    sc.port = "0";
    sc.text_addr = "127.0.0.1";
    sc.resp_port = "0";
    sc.memcached_port = "0";
    sc.store.capacity = 64u << 20;
    sc.store.shards = 8;
    sc.threads = 2;
    sc.log_level = AKVD_LOG_ERROR;
    int err;
    akvd *s = akvd_start(&sc, &err);
    EXPECT(s != NULL, "server: %d", err);
    sc.auth_file = cred;
    akvd *sa = akvd_start(&sc, &err);
    EXPECT(sa != NULL, "server with credentials: %d", err);
    if (s == NULL || sa == NULL) {
        return 1;
    }
    const pace_driver *drivers[3] = {pace_driver_find("resp"), pace_driver_find("memcached"),
                                     pace_driver_find("native")};
    for (int i = 0; i < 3; i++) {
        char port[16];
        port_of(drivers[i], s, port);
        test_sync(drivers[i], port);
        test_async(drivers[i], port);
        test_runner(drivers[i], port);
        char aport[16];
        port_of(drivers[i], sa, aport);
        pace_driver_set_auth("tester:wrong");
        pace_conn *c = drivers[i]->connect("127.0.0.1", aport);
        EXPECT(c == NULL, "%s: wrong secret refused", drivers[i]->name);
        if (c != NULL) {
            drivers[i]->close(c);
        }
        pace_driver_set_auth("tester:s3cret");
        c = drivers[i]->connect("127.0.0.1", aport);
        char buf[16];
        size_t n;
        EXPECT(c != NULL && drivers[i]->put(c, "x", 1, "y", 1) == PACE_OK &&
                   drivers[i]->get(c, "x", 1, buf, sizeof(buf), &n) == PACE_OK,
               "%s: authenticated session", drivers[i]->name);
        if (c != NULL) {
            drivers[i]->close(c);
        }
        pace_driver_set_auth(NULL);
    }
    akvd_stop(s);
    akvd_stop(sa);
    unlink(cred);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_drivers: ok");
    return 0;
}
#else
int main(void) {
    puts("test_drivers: skipped (build without the native driver)");
    return 0;
}
#endif
