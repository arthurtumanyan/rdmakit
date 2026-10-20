/*
 * test_resp.c - RESP driver test against an in-process server.
 *
 * The server thread implements SET and GET with a small table, parses the
 * array-of-bulk-strings request format and answers with +OK, bulk strings and
 * the null bulk string. It exists only to test the driver without external
 * services. The test covers value round trips, missing keys, binary-safe
 * values with embedded CRLF, large values, and the too-small-buffer error.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver.h"

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

#define TABLE_SLOTS 64
#define MAX_ARG (1 << 20)

struct entry {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
};

static struct entry table[TABLE_SLOTS];

static int read_byte(int fd) {
    unsigned char c;
    return read(fd, &c, 1) == 1 ? c : -1;
}

static int read_n(int fd, void *dst, size_t n) {
    unsigned char *d = dst;
    while (n > 0) {
        ssize_t r = read(fd, d, n);
        if (r <= 0) {
            return -1;
        }
        d += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Reads a decimal number terminated by CRLF after a prefix byte; returns -1 on error. */
static long read_num(int fd, int prefix) {
    if (read_byte(fd) != prefix) {
        return -1;
    }
    long v = 0;
    int c;
    while ((c = read_byte(fd)) >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
    }
    return (c == '\r' && read_byte(fd) == '\n') ? v : -1;
}

/* Reads one request into args; returns the argument count or -1. */
static int read_request(int fd, char **args, size_t *lens) {
    long argc = read_num(fd, '*');
    if (argc < 1 || argc > 3) {
        return -1;
    }
    for (long i = 0; i < argc; i++) {
        long len = read_num(fd, '$');
        if (len < 0 || len > MAX_ARG) {
            return -1;
        }
        args[i] = malloc((size_t)len + 2);
        lens[i] = (size_t)len;
        if (read_n(fd, args[i], (size_t)len + 2) != 0) {
            return -1;
        }
    }
    return (int)argc;
}

static void *server_main(void *arg) {
    int fd = (int)(long)arg;
    char *args[3];
    size_t lens[3];
    int argc;
    while ((argc = read_request(fd, args, lens)) > 0) {
        if (argc == 3 && memcmp(args[0], "SET", 3) == 0) {
            size_t slot = (unsigned char)args[1][0] % TABLE_SLOTS;
            free(table[slot].key);
            free(table[slot].val);
            table[slot].key = args[1];
            table[slot].klen = lens[1];
            table[slot].val = args[2];
            table[slot].vlen = lens[2];
            free(args[0]);
            if (write(fd, "+OK\r\n", 5) != 5) {
                break;
            }
        } else if (argc == 2 && memcmp(args[0], "GET", 3) == 0) {
            size_t slot = (unsigned char)args[1][0] % TABLE_SLOTS;
            struct entry *e = &table[slot];
            if (e->key != NULL && e->klen == lens[1] && memcmp(e->key, args[1], lens[1]) == 0) {
                char head[32];
                int n = snprintf(head, sizeof(head), "$%zu\r\n", e->vlen);
                if (write(fd, head, (size_t)n) != n || write(fd, e->val, e->vlen) != (ssize_t)e->vlen ||
                    write(fd, "\r\n", 2) != 2) {
                    break;
                }
            } else if (write(fd, "$-1\r\n", 5) != 5) {
                break;
            }
            free(args[0]);
            free(args[1]);
        } else if (write(fd, "-ERR unknown\r\n", 14) != 14) {
            break;
        }
    }
    close(fd);
    return NULL;
}

int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(ls, 1) != 0) {
        perror("listen");
        return 1;
    }
    socklen_t slen = sizeof(sa);
    getsockname(ls, (struct sockaddr *)&sa, &slen);
    char port[16];
    snprintf(port, sizeof(port), "%u", ntohs(sa.sin_port));

    const pace_driver *d = pace_driver_find("resp");
    EXPECT(d != NULL, "driver registered");
    pace_conn *c = d->connect("127.0.0.1", port);
    EXPECT(c != NULL, "connect");
    if (c == NULL) {
        return 1;
    }
    int fd = accept(ls, NULL, NULL);
    pthread_t th;
    pthread_create(&th, NULL, server_main, (void *)(long)fd);

    char buf[4096];
    size_t n = 0;
    EXPECT(d->get(c, "a-missing", 9, buf, sizeof(buf), &n) == PACE_NOTFOUND, "missing key");
    EXPECT(d->put(c, "alpha", 5, "hello", 5) == PACE_OK, "put");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == 5 && memcmp(buf, "hello", 5) == 0,
           "get after put");
    EXPECT(d->put(c, "alpha", 5, "x\r\nz", 4) == PACE_OK, "put value with CRLF");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == 4 && memcmp(buf, "x\r\nz", 4) == 0,
           "binary-safe value");
    EXPECT(d->put(c, "beta", 4, "", 0) == PACE_OK, "put empty value");
    EXPECT(d->get(c, "beta", 4, buf, sizeof(buf), &n) == PACE_OK && n == 0, "empty value");

    size_t big_len = 300000;
    char *big = malloc(big_len);
    char *back = malloc(big_len);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (char)(i * 31 + 7);
    }
    EXPECT(d->put(c, "gamma", 5, big, big_len) == PACE_OK, "put large value");
    EXPECT(d->get(c, "gamma", 5, back, big_len, &n) == PACE_OK && n == big_len &&
               memcmp(big, back, big_len) == 0,
           "large value round trip");
    EXPECT(d->get(c, "gamma", 5, buf, sizeof(buf), &n) == PACE_ERR, "value larger than buffer is an error");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == 4,
           "connection usable after oversized value");

    d->close(c);
    pthread_join(th, NULL);
    close(ls);
    free(big);
    free(back);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_resp: ok");
    return 0;
}
