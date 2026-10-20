/*
 * test_memcached.c - memcached driver test against an in-process server.
 *
 * The server thread implements the text commands set and get with a small table. The test covers value
 * round trips, missing keys, binary-safe values (embedded CRLF and an "END" line), empty values, large
 * values, and the error for a value larger than the caller's buffer, after which the connection must
 * remain usable.
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

#define SLOTS 64

struct entry {
    char key[64];
    char *val;
    size_t vlen;
};

static struct entry table[SLOTS];

static int read_byte(int fd) {
    unsigned char c;
    return read(fd, &c, 1) == 1 ? c : -1;
}

static int read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    int c;
    while ((c = read_byte(fd)) >= 0) {
        if (c == '\n') {
            if (n > 0 && out[n - 1] == '\r') {
                n--;
            }
            out[n] = '\0';
            return 0;
        }
        if (n + 1 < cap) {
            out[n++] = (char)c;
        }
    }
    return -1;
}

static int read_n(int fd, char *dst, size_t n) {
    while (n > 0) {
        ssize_t r = read(fd, dst, n);
        if (r <= 0) {
            return -1;
        }
        dst += r;
        n -= (size_t)r;
    }
    return 0;
}

static void *server_main(void *arg) {
    int fd = (int)(long)arg;
    char line[256];
    while (read_line(fd, line, sizeof(line)) == 0) {
        char key[64];
        unsigned flags;
        unsigned exptime;
        size_t bytes;
        struct entry *e;
        if (sscanf(line, "set %63s %u %u %zu", key, &flags, &exptime, &bytes) == 4) {
            char *data = malloc(bytes + 2);
            if (read_n(fd, data, bytes + 2) != 0) {
                break;
            }
            e = &table[(unsigned char)key[0] % SLOTS];
            free(e->val);
            snprintf(e->key, sizeof(e->key), "%s", key);
            e->val = data;
            e->vlen = bytes;
            if (write(fd, "STORED\r\n", 8) != 8) {
                break;
            }
        } else if (sscanf(line, "get %63s", key) == 1) {
            e = &table[(unsigned char)key[0] % SLOTS];
            if (e->val != NULL && strcmp(e->key, key) == 0) {
                char head[128];
                int n = snprintf(head, sizeof(head), "VALUE %s 0 %zu\r\n", key, e->vlen);
                if (write(fd, head, (size_t)n) != n || write(fd, e->val, e->vlen) != (ssize_t)e->vlen ||
                    write(fd, "\r\nEND\r\n", 7) != 7) {
                    break;
                }
            } else if (write(fd, "END\r\n", 5) != 5) {
                break;
            }
        } else if (write(fd, "ERROR\r\n", 7) != 7) {
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

    const pace_driver *d = pace_driver_find("memcached");
    EXPECT(d != NULL, "driver registered");
    if (d == NULL) {
        return 1;
    }
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
    EXPECT(d->get(c, "amissing", 8, buf, sizeof(buf), &n) == PACE_NOTFOUND, "missing key");
    EXPECT(d->put(c, "alpha", 5, "hello", 5) == PACE_OK, "put");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == 5 && memcmp(buf, "hello", 5) == 0,
           "get after put");
    static const char tricky[] = "x\r\nEND\r\nz";
    EXPECT(d->put(c, "alpha", 5, tricky, sizeof(tricky) - 1) == PACE_OK, "put value with CRLF and END");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == sizeof(tricky) - 1 &&
               memcmp(buf, tricky, n) == 0,
           "binary-safe value");
    EXPECT(d->put(c, "beta", 4, "", 0) == PACE_OK, "put empty value");
    EXPECT(d->get(c, "beta", 4, buf, sizeof(buf), &n) == PACE_OK && n == 0, "empty value");

    size_t big_len = 200000;
    char *big = malloc(big_len);
    char *back = malloc(big_len);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (char)(i * 31 + 7);
    }
    EXPECT(d->put(c, "gamma", 5, big, big_len) == PACE_OK, "put large value");
    EXPECT(d->get(c, "gamma", 5, back, big_len, &n) == PACE_OK && n == big_len &&
               memcmp(big, back, big_len) == 0,
           "large round trip");
    EXPECT(d->get(c, "gamma", 5, buf, sizeof(buf), &n) == PACE_ERR, "value larger than buffer is an error");
    EXPECT(d->get(c, "alpha", 5, buf, sizeof(buf), &n) == PACE_OK && n == sizeof(tricky) - 1,
           "usable after oversize value");

    d->close(c);
    pthread_join(th, NULL);
    close(ls);
    free(big);
    free(back);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_memcached: ok");
    return 0;
}
