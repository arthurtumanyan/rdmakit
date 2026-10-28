/*
 * test_sentinel.c - decision rules and request handling of the failover monitor (src/sentinel.h).
 *
 *   - replica choice: highest offset among reachable replicas, lowest index on a tie, never the primary
 *   - votes needed: a majority of all monitors and at least the quorum
 *   - votes: at most one per epoch, for the first requester; a newer epoch raises the current epoch
 *   - one monitor in this process (its nodes unreachable): SENTINEL GET-MASTER-ADDR-BY-NAME, MASTERS (the
 *     fields redis-py reads), REPLICAS, SENTINELS, MYID, an unknown group name; a vote request is granted
 *     once per epoch, and a monitor restarted from the same state file keeps its vote and identity
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "sentinel.h"

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

static void test_rules(void) {
    sentinel_candidate c[4] = {
        {.reachable = 1, .is_replica = 0, .offset = 900}, /* the old primary: never chosen */
        {.reachable = 1, .is_replica = 1, .offset = 500},
        {.reachable = 0, .is_replica = 1, .offset = 800}, /* unreachable */
        {.reachable = 1, .is_replica = 1, .offset = 700},
    };
    EXPECT(sentinel_pick(c, 4, 0) == 3, "highest reachable offset: %d", sentinel_pick(c, 4, 0));
    c[1].offset = 700;
    EXPECT(sentinel_pick(c, 4, 0) == 1, "tie goes to the lower index");
    c[1].reachable = 0;
    c[3].is_replica = 0;
    EXPECT(sentinel_pick(c, 4, 0) == -1, "no reachable replica");

    EXPECT(sentinel_votes_needed(3, 2) == 2 && sentinel_votes_needed(3, 3) == 3 &&
               sentinel_votes_needed(5, 2) == 3 && sentinel_votes_needed(1, 1) == 1 &&
               sentinel_votes_needed(4, 1) == 3,
           "votes needed");

    sentinel_vote v = {.current_epoch = 3};
    EXPECT(sentinel_vote_request(&v, 4, "aaaa") == 1 && v.voted_epoch == 4 &&
               strcmp(v.voted_for, "aaaa") == 0 && v.current_epoch == 4,
           "first request of an epoch gets the vote");
    EXPECT(sentinel_vote_request(&v, 4, "bbbb") == 0 && strcmp(v.voted_for, "aaaa") == 0,
           "one vote per epoch");
    EXPECT(sentinel_vote_request(&v, 3, "cccc") == 0 && v.voted_epoch == 4, "an older epoch gets nothing");
    EXPECT(sentinel_vote_request(&v, 6, "cccc") == 1 && v.voted_epoch == 6 && v.current_epoch == 6,
           "a newer epoch gets a new vote");
}

/* Sends one inline command to the monitor and returns the reply (static buffer). */
static const char *ask(int port, const char *cmd) {
    static char buf[8192];
    buf[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return buf;
    }
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char line[512];
    int n = snprintf(line, sizeof(line), "%s\r\n", cmd);
    if (send(fd, line, (size_t)n, 0) == n) {
        size_t got = 0;
        ssize_t r;
        /* replies end with CR LF; read until the peer pauses */
        while (got < sizeof(buf) - 1 && (r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0) {
            got += (size_t)r;
            buf[got] = '\0';
            if (got >= 2 && buf[got - 2] == '\r' && buf[got - 1] == '\n') {
                struct timeval shortv = {.tv_usec = 100000};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &shortv, sizeof(shortv));
            }
        }
    }
    close(fd);
    return buf;
}

static void test_monitor(void) {
    char dir[] = "/tmp/akv-sentinel-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        failures++;
        return;
    }
    sentinel_cfg cfg;
    sentinel_cfg_default(&cfg);
    snprintf(cfg.name, sizeof(cfg.name), "grp");
    snprintf(cfg.listen_addr, sizeof(cfg.listen_addr), "127.0.0.1");
    snprintf(cfg.listen_port, sizeof(cfg.listen_port), "0");
    snprintf(cfg.state_path, sizeof(cfg.state_path), "%s/state", dir);
    /* port 1 refuses connections: the nodes are unreachable, nothing is promoted */
    for (int i = 0; i < 3; i++) {
        snprintf(cfg.nodes[i].host, sizeof(cfg.nodes[i].host), "127.0.0.1");
        cfg.nodes[i].resp_port = 1;
        cfg.nodes[i].native_port = 2 + i;
    }
    cfg.nodes[1].resp_port = 3;
    cfg.nnodes = 3;
    snprintf(cfg.peers[0].host, sizeof(cfg.peers[0].host), "127.0.0.1");
    cfg.peers[0].port = 1;
    snprintf(cfg.peers[1].host, sizeof(cfg.peers[1].host), "127.0.0.1");
    cfg.peers[1].port = 1;
    cfg.npeers = 2;
    cfg.down_after_ms = 60000; /* no failover attempt during the test */
    char err[256];
    sentinel *s = sentinel_start(&cfg, err, sizeof(err));
    EXPECT(s != NULL, "start: %s", err);
    if (s == NULL) {
        return;
    }
    int port = sentinel_port(s);
    EXPECT(strcmp(ask(port, "SENTINEL get-master-addr-by-name grp"),
                  "*2\r\n$9\r\n127.0.0.1\r\n$1\r\n1\r\n") == 0,
           "primary address: %s", ask(port, "SENTINEL get-master-addr-by-name grp"));
    EXPECT(strcmp(ask(port, "SENTINEL get-master-addr-by-name other"), "*-1\r\n") == 0, "unknown group");
    const char *m = ask(port, "SENTINEL MASTERS");
    EXPECT(strncmp(m, "*1\r\n*22\r\n", 9) == 0 && strstr(m, "$4\r\nname\r\n$3\r\ngrp\r\n") &&
               strstr(m, "$5\r\nflags\r\n$6\r\nmaster\r\n") &&
               strstr(m, "$19\r\nnum-other-sentinels\r\n$1\r\n2\r\n") &&
               strstr(m, "$6\r\nquorum\r\n$1\r\n2\r\n"),
           "SENTINEL MASTERS: %s", m);
    const char *r = ask(port, "SENTINEL REPLICAS grp");
    EXPECT(strncmp(r, "*2\r\n*16\r\n", 9) == 0 &&
               strstr(r, "$2\r\nip\r\n$9\r\n127.0.0.1\r\n$4\r\nport\r\n$1\r\n3\r\n"),
           "SENTINEL REPLICAS: %s", r);
    EXPECT(strncmp(ask(port, "SENTINEL SENTINELS grp"), "*2\r\n*10\r\n", 9) == 0, "SENTINEL SENTINELS");
    EXPECT(strncmp(ask(port, "SENTINEL MASTER nope"), "-ERR No such master", 19) == 0, "unknown name error");
    char id[64];
    snprintf(id, sizeof(id), "%s", ask(port, "SENTINEL MYID"));
    EXPECT(strncmp(id, "$40\r\n", 5) == 0, "identity: %s", id);

    const char *a = "1111111111111111111111111111111111111111";
    const char *b = "2222222222222222222222222222222222222222";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SENTINEL is-master-down-by-addr 127.0.0.1 1 7 %s", a);
    EXPECT(strcmp(ask(port, cmd), "*3\r\n:0\r\n$40\r\n1111111111111111111111111111111111111111\r\n:7\r\n") ==
               0,
           "vote granted: %s", ask(port, cmd));
    snprintf(cmd, sizeof(cmd), "SENTINEL is-master-down-by-addr 127.0.0.1 1 7 %s", b);
    EXPECT(strstr(ask(port, cmd), a) != NULL, "second requester of epoch 7 gets no vote");
    sentinel_stop(s);

    /* restart from the same state file: identity and the vote of epoch 7 are kept */
    s = sentinel_start(&cfg, err, sizeof(err));
    EXPECT(s != NULL, "restart: %s", err);
    if (s == NULL) {
        return;
    }
    port = sentinel_port(s);
    EXPECT(strcmp(ask(port, "SENTINEL MYID"), id) == 0, "identity kept across a restart");
    EXPECT(strstr(ask(port, cmd), a) != NULL, "vote of epoch 7 kept across a restart");
    snprintf(cmd, sizeof(cmd), "SENTINEL is-master-down-by-addr 127.0.0.1 1 8 %s", b);
    EXPECT(strstr(ask(port, cmd), b) != NULL, "epoch 8 is a new vote");
    sentinel_stop(s);
    char rm[300];
    snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    if (system(rm) != 0) {
        failures++;
    }
}

int main(void) {
    test_rules();
    test_monitor();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_sentinel: ok");
    return 0;
}
