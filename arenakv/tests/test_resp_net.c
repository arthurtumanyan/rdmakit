/*
 * test_resp_net.c - publish/subscribe and transactions over RESP on real sockets, with several workers.
 *
 * - SUBSCRIBE and PSUBSCRIBE confirmations; messages to RESP2 subscribers (arrays), RESP3 subscribers
 *   (pushes) and pattern subscribers (pmessage); PUBLISH counts the receivers
 * - a RESP2 session with subscriptions accepts only the pub/sub commands, PING (answered as a pub/sub
 *   frame), QUIT and RESET; a RESP3 session accepts every command
 * - PUBSUB CHANNELS, NUMSUB and NUMPAT; UNSUBSCRIBE without arguments; a closed subscriber is removed
 * - a subscriber that does not read is disconnected when its pending messages exceed the output limit
 * - eight publishing threads and 64 subscribers spread over the workers: every subscriber receives every
 *   message exactly once (run under ThreadSanitizer as well)
 * - transactions on several connections at once: MULTI INCR a INCR b EXEC returns equal values every time,
 *   which fails if another client's increments can interleave with a transaction
 * - MSET is atomic: a reader that reads both keys in a transaction never sees one MSET half applied
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "akvd.h"

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

static int port;

static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void sendstr(int fd, const char *s) {
    size_t n = strlen(s);
    if (write(fd, s, n) != (ssize_t)n) {
        EXPECT(0, "write");
    }
}

/* Reads exactly strlen(want) bytes (waiting up to 3 s) and compares them with want. */
static int expect_bytes(int fd, const char *want) {
    size_t n = strlen(want);
    char *got = calloc(1, n + 1);
    size_t have = 0;
    while (have < n) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 3000) <= 0) {
            break;
        }
        ssize_t r = read(fd, got + have, n - have);
        if (r <= 0) {
            break;
        }
        have += (size_t)r;
    }
    int ok = have == n && memcmp(got, want, n) == 0;
    EXPECT(ok, "expected [%s]\n  got [%.*s]", want, (int)have, got);
    free(got);
    return ok;
}

/* Sends a command and checks the reply. */
static void call(int fd, const char *cmd, const char *want) {
    sendstr(fd, cmd);
    expect_bytes(fd, want);
}

/* Publishes until the receiver count equals want (subscriptions of other connections become effective
 * asynchronously only on close). Returns the last count. */
static long publish_until(int fd, const char *ch, long want) {
    long got = -1;
    for (int i = 0; i < 100 && got != want; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "PUBLISH %s probe\r\n", ch);
        sendstr(fd, cmd);
        char buf[64];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        if (r <= 0) {
            break;
        }
        buf[r] = '\0';
        got = strtol(buf + 1, NULL, 10);
        if (got != want) {
            struct timespec ts = {.tv_nsec = 20000000};
            nanosleep(&ts, NULL);
        }
    }
    return got;
}

/* Drains whatever a socket holds (probe messages of publish_until). */
static void drain(int fd) {
    char buf[4096];
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (poll(&p, 1, 200) > 0 && read(fd, buf, sizeof(buf)) > 0) {
    }
}

static void test_basic(void) {
    int a = dial();
    int b = dial();
    int c = dial();
    int pub = dial();
    call(a, "SUBSCRIBE news\r\n", "*3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n");
    call(b, "HELLO 3\r\n", ""); /* the map reply is read below */
    drain(b);
    call(b, "SUBSCRIBE news\r\n", ">3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n");
    call(c, "PSUBSCRIBE ne*\r\n", "*3\r\n$10\r\npsubscribe\r\n$3\r\nne*\r\n:1\r\n");
    call(pub, "PUBLISH news hello\r\n", ":3\r\n");
    expect_bytes(a, "*3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$5\r\nhello\r\n");
    expect_bytes(b, ">3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$5\r\nhello\r\n");
    expect_bytes(c, "*4\r\n$8\r\npmessage\r\n$3\r\nne*\r\n$4\r\nnews\r\n$5\r\nhello\r\n");
    call(pub, "PUBLISH other x\r\n", ":0\r\n");

    call(a, "GET k\r\n",
         "-ERR Can't execute 'get': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed "
         "in "
         "this context\r\n");
    call(a, "PING\r\n", "*2\r\n$4\r\npong\r\n$0\r\n\r\n");
    call(b, "GET k\r\n", "_\r\n");

    call(pub, "PUBSUB NUMSUB news other\r\n", "*4\r\n$4\r\nnews\r\n:2\r\n$5\r\nother\r\n:0\r\n");
    call(pub, "PUBSUB CHANNELS\r\n", "*1\r\n$4\r\nnews\r\n");
    call(pub, "PUBSUB NUMPAT\r\n", ":1\r\n");

    call(a, "UNSUBSCRIBE\r\n", "*3\r\n$11\r\nunsubscribe\r\n$4\r\nnews\r\n:0\r\n");
    call(a, "UNSUBSCRIBE\r\n", "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n");
    call(a, "GET k\r\n", "$-1\r\n");
    call(pub, "PUBLISH news again\r\n", ":2\r\n");
    drain(b);
    drain(c);
    close(b);
    EXPECT(publish_until(pub, "news", 1) == 1, "a closed subscriber is removed");
    drain(c); /* the probes of publish_until */
    call(c, "RESET\r\n", "+RESET\r\n");
    EXPECT(publish_until(pub, "news", 0) == 0, "RESET removes the subscriptions");
    close(a);
    close(c);
    close(pub);
}

static void test_slow_subscriber(void) {
    int s = dial();
    int pub = dial();
    call(s, "SUBSCRIBE flood\r\n", "*3\r\n$9\r\nsubscribe\r\n$5\r\nflood\r\n:1\r\n");
    char *cmd = malloc(9000);
    int n = snprintf(cmd, 9000, "*3\r\n$7\r\nPUBLISH\r\n$5\r\nflood\r\n$8192\r\n");
    memset(cmd + n, 'x', 8192);
    memcpy(cmd + n + 8192, "\r\n", 2);
    size_t len = (size_t)n + 8194;
    long last = -1;
    char buf[64];
    for (int i = 0; i < 4000 && last != 0; i++) {
        if (write(pub, cmd, len) != (ssize_t)len) {
            break;
        }
        ssize_t r = read(pub, buf, sizeof(buf) - 1);
        if (r <= 0) {
            break;
        }
        buf[r] = '\0';
        last = strtol(buf + 1, NULL, 10);
    }
    EXPECT(last == 0, "the subscriber was disconnected (receivers %ld)", last);
    /* its socket ends: read what was delivered until the end of the stream */
    char tmp[65536];
    ssize_t r;
    struct pollfd p = {.fd = s, .events = POLLIN};
    int eof = 0;
    while (poll(&p, 1, 3000) > 0 && (r = read(s, tmp, sizeof(tmp))) >= 0) {
        if (r == 0) {
            eof = 1;
            break;
        }
    }
    EXPECT(eof, "the slow subscriber's connection is closed");
    free(cmd);
    close(s);
    close(pub);
}

enum { SUBSCRIBERS = 64, PUBLISHERS = 8, MESSAGES = 500 };

struct reader {
    int fd;
    unsigned received;
};

static void *reader_main(void *arg) {
    struct reader *r = arg;
    static const char frame[] = "*3\r\n$7\r\nmessage\r\n$6\r\nstress\r\n$4\r\nbody\r\n";
    size_t flen = sizeof(frame) - 1;
    char buf[65536];
    size_t carry = 0;
    while (r->received < PUBLISHERS * MESSAGES) {
        struct pollfd p = {.fd = r->fd, .events = POLLIN};
        if (poll(&p, 1, 5000) <= 0) {
            break;
        }
        ssize_t n = read(r->fd, buf + carry, sizeof(buf) - carry);
        if (n <= 0) {
            break;
        }
        size_t have = carry + (size_t)n;
        size_t off = 0;
        while (have - off >= flen && memcmp(buf + off, frame, flen) == 0) {
            r->received++;
            off += flen;
        }
        if (have - off >= flen) {
            break; /* not a message frame: corrupted stream */
        }
        memmove(buf, buf + off, have - off);
        carry = have - off;
    }
    return NULL;
}

static void *publisher_main(void *arg) {
    (void)arg;
    int fd = dial();
    char buf[64];
    for (int i = 0; i < MESSAGES; i++) {
        sendstr(fd, "PUBLISH stress body\r\n");
        if (read(fd, buf, sizeof(buf)) <= 0) {
            break;
        }
    }
    close(fd);
    return NULL;
}

static void test_stress(void) {
    struct reader rd[SUBSCRIBERS];
    pthread_t rt[SUBSCRIBERS];
    for (int i = 0; i < SUBSCRIBERS; i++) {
        rd[i] = (struct reader){.fd = dial()};
        call(rd[i].fd, "SUBSCRIBE stress\r\n", "*3\r\n$9\r\nsubscribe\r\n$6\r\nstress\r\n:1\r\n");
    }
    for (int i = 0; i < SUBSCRIBERS; i++) {
        pthread_create(&rt[i], NULL, reader_main, &rd[i]);
    }
    pthread_t pt[PUBLISHERS];
    for (int i = 0; i < PUBLISHERS; i++) {
        pthread_create(&pt[i], NULL, publisher_main, NULL);
    }
    for (int i = 0; i < PUBLISHERS; i++) {
        pthread_join(pt[i], NULL);
    }
    unsigned total = 0;
    unsigned exact = 0;
    for (int i = 0; i < SUBSCRIBERS; i++) {
        pthread_join(rt[i], NULL);
        total += rd[i].received;
        exact += rd[i].received == PUBLISHERS * MESSAGES;
        close(rd[i].fd);
    }
    EXPECT(exact == SUBSCRIBERS, "%u of %d subscribers received all %d messages (%u in total)", exact,
           SUBSCRIBERS, PUBLISHERS * MESSAGES, total);
    printf("stress: %d publishers x %d messages to %d subscribers: %u frames delivered\n", PUBLISHERS,
           MESSAGES, SUBSCRIBERS, total);
}

enum { TXN_CLIENTS = 6, TXN_ROUNDS = 2000 };

static void *txn_main(void *arg) {
    unsigned *bad = arg;
    int fd = dial();
    char buf[256];
    for (int i = 0; i < TXN_ROUNDS; i++) {
        sendstr(fd, "MULTI\r\nINCR a\r\nINCR b\r\nEXEC\r\n");
        size_t have = 0;
        int lines = 0;
        while (lines < 6 && have < sizeof(buf) - 1) {
            ssize_t r = read(fd, buf + have, sizeof(buf) - 1 - have);
            if (r <= 0) {
                break;
            }
            for (ssize_t k = 0; k < r; k++) {
                lines += buf[have + (size_t)k] == '\n';
            }
            have += (size_t)r;
        }
        buf[have] = '\0';
        long x = -1;
        long y = -2;
        if (sscanf(buf, "+OK\r\n+QUEUED\r\n+QUEUED\r\n*2\r\n:%ld\r\n:%ld\r\n", &x, &y) != 2 || x != y) {
            (*bad)++;
        }
    }
    close(fd);
    return NULL;
}

static void test_transactions(void) {
    pthread_t th[TXN_CLIENTS];
    unsigned bad[TXN_CLIENTS] = {0};
    for (int i = 0; i < TXN_CLIENTS; i++) {
        pthread_create(&th[i], NULL, txn_main, &bad[i]);
    }
    unsigned total = 0;
    for (int i = 0; i < TXN_CLIENTS; i++) {
        pthread_join(th[i], NULL);
        total += bad[i];
    }
    EXPECT(total == 0, "%u of %d transactions saw interleaved increments", total, TXN_CLIENTS * TXN_ROUNDS);
    int fd = dial();
    char want[64];
    snprintf(want, sizeof(want), "$%d\r\n%d\r\n", TXN_CLIENTS * TXN_ROUNDS >= 10000 ? 5 : 4,
             TXN_CLIENTS * TXN_ROUNDS);
    call(fd, "GET a\r\n", want);
    close(fd);
}

/* Writers run MSET ma i mb i; readers read both in one transaction: an MSET is never seen half done. */
static atomic_int mset_stop;

static void *mset_writer(void *arg) {
    (void)arg;
    int fd = dial();
    char cmd[128];
    char buf[64];
    for (int i = 0; i < 3000; i++) {
        snprintf(cmd, sizeof(cmd), "MSET ma %d mb %d\r\n", i, i);
        sendstr(fd, cmd);
        if (read(fd, buf, sizeof(buf)) <= 0) {
            break;
        }
    }
    close(fd);
    return NULL;
}

static void *mset_reader(void *arg) {
    unsigned *bad = arg;
    int fd = dial();
    char buf[256];
    while (!atomic_load(&mset_stop)) {
        sendstr(fd, "MULTI\r\nMGET ma mb\r\nEXEC\r\n");
        size_t have = 0;
        int lines = 0;
        while (lines < 8 && have < sizeof(buf) - 1) {
            ssize_t r = read(fd, buf + have, sizeof(buf) - 1 - have);
            if (r <= 0) {
                break;
            }
            for (ssize_t k = 0; k < r; k++) {
                lines += buf[have + (size_t)k] == '\n';
            }
            have += (size_t)r;
        }
        buf[have] = '\0';
        char x[16];
        char y[16];
        if (sscanf(buf, "+OK\r\n+QUEUED\r\n*1\r\n*2\r\n$%*d\r\n%15[0-9]\r\n$%*d\r\n%15[0-9]", x, y) == 2 &&
            strcmp(x, y) != 0) {
            (*bad)++;
        }
    }
    close(fd);
    return NULL;
}

static void test_mset_atomic(void) {
    int fd = dial();
    call(fd, "MSET ma 0 mb 0\r\n", "+OK\r\n");
    close(fd);
    pthread_t w[3];
    pthread_t r;
    unsigned bad = 0;
    atomic_store(&mset_stop, 0);
    pthread_create(&r, NULL, mset_reader, &bad);
    for (int i = 0; i < 3; i++) {
        pthread_create(&w[i], NULL, mset_writer, NULL);
    }
    for (int i = 0; i < 3; i++) {
        pthread_join(w[i], NULL);
    }
    atomic_store(&mset_stop, 1);
    pthread_join(r, NULL);
    EXPECT(bad == 0, "%u reads saw an MSET half done", bad);
}

static akvd *start(uint64_t limit) {
    akvd_config cfg;
    akvd_config_default(&cfg);
    cfg.port = NULL;
    cfg.text_addr = "127.0.0.1";
    cfg.resp_port = "0";
    cfg.threads = 4;
    cfg.store.capacity = 8u << 20;
    cfg.store.shards = 4;
    cfg.pubsub_output_limit = limit;
    cfg.log_level = AKVD_LOG_ERROR;
    int err = 0;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "start: %d", err);
    port = d != NULL ? akvd_resp_port(d) : 0;
    return d;
}

int main(void) {
    akvd *d = start(0);
    if (d != NULL) {
        test_basic();
        test_stress();
        test_transactions();
        test_mset_atomic();
        akvd_stop(d);
    }
    d = start(256u << 10);
    if (d != NULL) {
        test_slow_subscriber();
        akvd_stop(d);
    }
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_resp_net: ok");
    return 0;
}
