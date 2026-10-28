/*
 * test_proto.c - memcached and RESP protocol modules, without sockets.
 *
 * A server without listeners provides the engine and counters. Each case feeds a request into the
 * protocol module and compares the reply byte for byte. Every script is also fed one byte at a time:
 * the replies must be identical to those of the whole buffer (incremental parsing). A random fuzz feeds
 * one million inputs mixing valid commands, mutations and noise; the modules must consume no more than
 * offered, never crash (run under the sanitizers as well) and keep the engine consistent.
 * RESP3 reply types after HELLO 3. Transactions: queued commands, EXECABORT after a refused command, errors
 * of single commands inside EXEC, WATCH conflicts from another session, nesting errors, a queue that needs
 * every shard locked.
 * Access control: users of an ACL file with command, category and key restrictions in all three
 * protocols, the ACL subcommands, and ACL LOAD (disabled users lose their connection, a damaged file is
 * refused and the old rules stay in force).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akvproto.h"
#include "srv.h"

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

typedef size_t (*exec_fn)(akvd *, struct session *, const uint8_t *, size_t, struct obuf *);

/* Feeds in[0, n) and returns the replies (NUL-terminated, caller frees). bytewise feeds one byte at a time. */
static char *feed(akvd *d, exec_fn fn, struct session *ss, const char *in, size_t n, int bytewise) {
    struct obuf out = {0};
    struct obuf buf = {0};
    for (size_t i = 0; i < n && !ss->close;) {
        size_t step = bytewise ? 1 : n - i;
        ob_add(&buf, in + i, step);
        i += step;
        size_t used = fn(d, ss, buf.p + buf.off, buf.len - buf.off, &out);
        buf.off += used;
        ob_compact(&buf);
    }
    ob_add(&out, "", 1);
    ob_free(&buf);
    char *r = strdup((const char *)out.p + out.off);
    ob_free(&out);
    return r;
}

/* Runs a request on a fresh session, whole and byte by byte, and compares both with want (NULL = no check). */
static char *run(akvd *d, exec_fn fn, const char *in, const char *want, int authed) {
    struct session s1 = {.authed = authed};
    char *whole = feed(d, fn, &s1, in, strlen(in), 0);
    resp_session_free(&s1);
    if (want != NULL) {
        EXPECT(strcmp(whole, want) == 0, "request %.60s\n  got  [%s]\n  want [%s]", in, whole, want);
    }
    return whole;
}

/* As run() for requests that are idempotent: the byte-by-byte feed must give the same replies. */
static void run2(akvd *d, exec_fn fn, const char *in, const char *want) {
    char *a = run(d, fn, in, want, 1);
    struct session s2 = {.authed = 1};
    char *b = feed(d, fn, &s2, in, strlen(in), 1);
    resp_session_free(&s2);
    EXPECT(strcmp(a, b) == 0, "incremental feed differs for %.60s\n  whole [%s]\n  bytes [%s]", in, a, b);
    free(a);
    free(b);
}

static void test_memcached(akvd *d) {
    exec_fn m = mc_execute;
    akv_flush(akvd_store(d));
    free(run(d, m, "set a 5 0 3\r\nabc\r\nget a\r\n", "STORED\r\nVALUE a 5 3\r\nabc\r\nEND\r\n", 1));
    free(run(d, m, "set far 0 9223372036854775807 1\r\nx\r\nget far\r\n",
             "STORED\r\nVALUE far 0 1\r\nx\r\nEND\r\n", 1)); /* absolute exptime at the limit */
    run2(d, m, "get a b\r\n", "VALUE a 5 3\r\nabc\r\nEND\r\n");
    run2(d, m, "add a 0 0 1\r\nx\r\n", "NOT_STORED\r\n");
    run2(d, m, "replace zz 0 0 1\r\nx\r\n", "NOT_STORED\r\n");
    free(run(d, m, "append a 0 0 2\r\nde\r\nprepend a 0 0 2\r\n12\r\nget a\r\n",
             "STORED\r\nSTORED\r\nVALUE a 5 7\r\n12abcde\r\nEND\r\n", 1));
    free(run(d, m, "set n 0 0 2\r\n10\r\nincr n 5\r\ndecr n 100\r\nincr zz 1\r\n",
             "STORED\r\n15\r\n0\r\nNOT_FOUND\r\n", 1));
    run2(d, m, "incr a 1\r\n", "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n");
    run2(d, m, "incr n x\r\n", "CLIENT_ERROR invalid numeric delta argument\r\n");
    free(run(d, m, "set q 0 0 1 noreply\r\nq\r\ndelete q noreply\r\nget q\r\n", "END\r\n", 1));
    run2(d, m, "delete nothere\r\n", "NOT_FOUND\r\n");
    free(run(d, m, "set t 0 100 1\r\nv\r\ntouch t 0\r\ntouch none 5\r\n",
             "STORED\r\nTOUCHED\r\nNOT_FOUND\r\n", 1));
    free(run(d, m, "set e 0 -1 1\r\nv\r\nget e\r\n", "STORED\r\nEND\r\n", 1));

    /* gets and cas */
    char *r = run(d, m, "gets a\r\n", NULL, 1);
    unsigned long long cas = 0;
    EXPECT(sscanf(r, "VALUE a 5 7 %llu", &cas) == 1 && cas != 0, "gets reports a cas: %s", r);
    free(r);
    char req[256];
    snprintf(req, sizeof(req), "cas a 1 0 1 %llu\r\nX\r\ncas a 1 0 1 %llu\r\nY\r\ncas none 0 0 1 1\r\nZ\r\n",
             cas, cas);
    free(run(d, m, req, "STORED\r\nEXISTS\r\nNOT_FOUND\r\n", 1));

    /* gat updates the expiry */
    free(run(d, m, "gat 100 a\r\n", "VALUE a 1 1\r\nX\r\nEND\r\n", 1));
    akv_meta meta;
    size_t vlen;
    uint8_t one;
    EXPECT(akv_get_meta(akvd_store(d), "a", 1, &one, 1, &vlen, &meta, AKV_TTL_KEEP) == 0 &&
               meta.ttl_ms > 99000,
           "gat set the ttl");

    /* errors and framing */
    run2(d, m, "bogus\r\n", "ERROR\r\n");
    run2(d, m, "get\r\n", "ERROR\r\n");
    run2(d, m, "\r\n", "ERROR\r\n");
    struct session s = {.authed = 1};
    char *x = feed(d, m, &s, "set k 0 0 3\r\nabcd\r\n", 18, 0);
    EXPECT(strcmp(x, "CLIENT_ERROR bad data chunk\r\n") == 0 && s.close, "bad data chunk closes: %s", x);
    free(x);
    s = (struct session){.authed = 1};
    x = feed(d, m, &s, "set k 0 0 x\r\n", 13, 0);
    EXPECT(s.close, "unparseable storage line closes the connection");
    free(x);
    /* oversized data block: skipped, connection stays usable */
    size_t big = akvd_max_entry(d) + 10;
    char *msg = malloc(big + 200);
    int hl = snprintf(msg, 200, "set big 0 0 %zu\r\n", big);
    memset(msg + hl, 'z', big);
    memcpy(msg + hl + big, "\r\nget a\r\n", 9);
    s = (struct session){.authed = 1};
    x = feed(d, m, &s, msg, (size_t)hl + big + 9, 0);
    EXPECT(strcmp(x, "SERVER_ERROR object too large for cache\r\nVALUE a 1 1\r\nX\r\nEND\r\n") == 0 &&
               !s.close,
           "oversized value skipped: %.80s", x);
    free(x);
    s = (struct session){.authed = 1};
    x = feed(d, m, &s, msg, (size_t)hl + big + 9, 1);
    EXPECT(strcmp(x, "SERVER_ERROR object too large for cache\r\nVALUE a 1 1\r\nX\r\nEND\r\n") == 0,
           "oversized value skipped bytewise: %.80s", x);
    free(x);
    free(msg);
    s = (struct session){.authed = 1};
    x = feed(d, m, &s, "quit\r\nget a\r\n", 12, 0);
    EXPECT(s.close && x[0] == '\0', "quit closes without reply");
    free(x);

    /* meta commands */
    free(run(d, m, "ms mk 3 T100 F7\r\nabc\r\n", "HD\r\n", 1));
    run2(d, m, "mg mk v f s\r\n", "VA 3 f7 s3\r\nabc\r\n");
    run2(d, m, "mg mk k Oop\r\n", "HD kmk Oop\r\n");
    run2(d, m, "mg missing v\r\n", "EN\r\n");
    run2(d, m, "mg missing v q\r\nmn\r\n", "MN\r\n");
    free(run(d, m, "ms mk 1 ME\r\nz\r\n", "NS\r\n", 1));
    free(run(d, m, "ms mk 2 MA\r\nde\r\nmg mk v\r\n", "HD\r\nVA 5\r\nabcde\r\n", 1));
    free(run(d, m, "ms cnt 1\r\n5\r\nma cnt v\r\nma cnt MD D10 v\r\nma none\r\nma none N0 J7 v\r\n",
             "HD\r\nVA 1\r\n6\r\nVA 1\r\n0\r\nNF\r\nVA 1\r\n7\r\n", 1));
    run2(d, m, "md none q\r\nmd none\r\n", "NF\r\n");
    run2(d, m, "md none q\r\n", "");
    free(run(d, m, "md mk\r\nmg mk v\r\n", "HD\r\nEN\r\n", 1));
    run2(d, m, "mg mk zz\r\n", "CLIENT_ERROR bad command line format\r\n");

    r = run(d, m, "stats\r\n", NULL, 1);
    EXPECT(strstr(r, "STAT curr_items ") != NULL && strstr(r, "STAT cmd_get ") != NULL &&
               strstr(r, "END\r\n") != NULL,
           "stats output");
    free(r);
    r = run(d, m, "version\r\n", NULL, 1);
    EXPECT(strncmp(r, "VERSION 1.6.0-arenakv-", 22) == 0, "version: %s", r);
    free(r);
    free(run(d, m, "flush_all\r\nget n\r\n", "OK\r\nEND\r\n", 1));
}

static void test_resp(akvd *d) {
    exec_fn r = resp_execute;
    akv_flush(akvd_store(d));
    free(run(d, r, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nabc\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
             "+OK\r\n$3\r\nabc\r\n", 1));
    run2(d, r, "GET k\r\n", "$3\r\nabc\r\n");
    run2(d, r, "GET nothere\r\n", "$-1\r\n");
    run2(d, r, "PING\r\n*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nPING\r\n$2\r\nhi\r\n",
         "+PONG\r\n+PONG\r\n$2\r\nhi\r\n");
    run2(d, r, "*2\r\n$4\r\nECHO\r\n$0\r\n\r\n", "$0\r\n\r\n");
    free(run(d, r, "SET k v NX\r\nSET k2 v XX\r\nSET k3 v NX\r\n", "$-1\r\n$-1\r\n+OK\r\n", 1));
    free(run(d, r, "SET k new GET\r\nGET k\r\n", "$3\r\nabc\r\n$3\r\nnew\r\n", 1));
    free(run(d, r, "MSET a 1 b 2\r\nMGET a b c\r\n", "+OK\r\n*3\r\n$1\r\n1\r\n$1\r\n2\r\n$-1\r\n", 1));
    free(run(d, r, "INCR a\r\nINCRBY a 10\r\nDECR a\r\nDECRBY a -3\r\nINCR fresh\r\n",
             ":2\r\n:12\r\n:11\r\n:14\r\n:1\r\n", 1));
    run2(d, r, "INCR k\r\n", "-ERR value is not an integer or out of range\r\n");
    /* DUMP and RESTORE: flags and time to live travel with the value; the payload is checksummed */
    {
        char *dump = run(d, r, "SET dk hello PX 50000\r\nDUMP dk\r\n", NULL, 1);
        EXPECT(strncmp(dump, "+OK\r\n$17\r\nAKV\x01", 13) == 0, "DUMP payload: %.12s", dump);
        free(dump);
        /* the payload of value "hello" with flags 7, built from the documented format */
        uint8_t payload[17] = {'A', 'K', 'V', 1, 7, 0, 0, 0, 'h', 'e', 'l', 'l', 'o'};
        uint32_t crc = akv_crc32c(0, payload, 13);
        for (int i = 0; i < 4; i++) {
            payload[13 + i] = (uint8_t)(crc >> (8 * i));
        }
        char req[256];
        int rl = snprintf(req, sizeof(req), "*4\r\n$7\r\nRESTORE\r\n$2\r\nd2\r\n$5\r\n30000\r\n$17\r\n");
        memcpy(req + rl, payload, 17);
        memcpy(req + rl + 17, "\r\n", 2);
        size_t reqlen = (size_t)rl + 19;
        struct session s1 = {.authed = 1};
        char *out = feed(d, r, &s1, req, reqlen, 0);
        EXPECT(strcmp(out, "+OK\r\n") == 0, "RESTORE: %s", out);
        free(out);
        out = feed(d, r, &s1, req, reqlen, 0);
        EXPECT(strncmp(out, "-BUSYKEY", 8) == 0, "RESTORE of an existing key: %s", out);
        free(out);
        req[rl + 9] ^= 1; /* one bit of the value: the checksum no longer matches */
        memcpy(strstr(req, "d2"), "d3", 2);
        out = feed(d, r, &s1, req, reqlen, 0);
        EXPECT(strncmp(out, "-ERR DUMP payload", 17) == 0, "damaged payload: %s", out);
        free(out);
        resp_session_free(&s1);
        char *t = run(d, r, "GET d2\r\nPTTL d2\r\nEXISTS d3\r\nDUMP nothere\r\n", NULL, 1);
        long long pttl = -5;
        char *ttl = strchr(t + 11, ':');
        EXPECT(strncmp(t, "$5\r\nhello\r\n:", 12) == 0 && ttl != NULL && sscanf(ttl, ":%lld", &pttl) == 1 &&
                   pttl > 0 && pttl <= 30000 && strstr(t, ":0\r\n$-1\r\n") != NULL,
               "restored value and time to live: %s", t);
        free(t);
        free(run(d, mc_execute, "get d2\r\n", "VALUE d2 7 5\r\nhello\r\nEND\r\n", 1)); /* flags kept */
        t = run(d, r, "RESTORE d4 1 x ABSTTL\r\n", NULL, 1);
        EXPECT(strncmp(t, "-ERR DUMP payload", 17) == 0, "short payload: %s", t);
        free(t);
        run2(d, r, "ASKING\r\n", "-ERR This instance has cluster support disabled\r\n");
    }
    /* negative integers and the most negative one through the integer reply writer */
    free(run(d, r, "SET m -9223372036854775807\r\nDECR m\r\nSET z 0\r\nDECRBY z 5\r\n",
             "+OK\r\n:-9223372036854775808\r\n+OK\r\n:-5\r\n", 1));
    /* a pipelined batch longer than the prefetch group, with keys prefetched for GET, SET, MGET and DEL
     * and commands without keys in between; fed whole and byte by byte (incomplete commands are skipped
     * by the prefetch scan and parsed once complete) */
    free(run(d, r,
             "*3\r\n$3\r\nSET\r\n$2\r\np1\r\n$1\r\na\r\n*3\r\n$3\r\nset\r\n$2\r\np2\r\n$2\r\nbb\r\n"
             "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$2\r\np1\r\n*4\r\n$4\r\nMGET\r\n$2\r\np1\r\n$2\r\np2\r\n"
             "$2\r\np3\r\n*2\r\n$3\r\nDEL\r\n$2\r\np1\r\n*2\r\n$3\r\nGET\r\n$2\r\np1\r\n*2\r\n$3\r\nget\r\n"
             "$2\r\np2\r\n*2\r\n$6\r\nSTRLEN\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$0\r\n\r\n",
             "+OK\r\n+OK\r\n+PONG\r\n$1\r\na\r\n*3\r\n$1\r\na\r\n$2\r\nbb\r\n$-1\r\n:1\r\n$-1\r\n$"
             "2\r\nbb\r\n:2\r\n"
             "$-1\r\n",
             1));
    run2(d, r,
         "*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np2\r\n"
         "*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np2\r\n"
         "*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np2\r\n*2\r\n$3\r\nGET\r\n$2\r\np9\r\n",
         "$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$2\r\nbb\r\n$-"
         "1\r\n");
    free(run(d, r, "SET big 9223372036854775807\r\nINCR big\r\n",
             "+OK\r\n-ERR increment or decrement would overflow\r\n", 1));
    free(run(d, r, "APPEND s ab\r\nAPPEND s cd\r\nSTRLEN s\r\nGET s\r\n", ":2\r\n:4\r\n:4\r\n$4\r\nabcd\r\n",
             1));
    run2(d, r, "EXISTS a b nothere a\r\nTYPE a\r\nTYPE nothere\r\n", ":3\r\n+string\r\n+none\r\n");
    free(run(d, r, "DEL a b nothere\r\nEXISTS a\r\n", ":2\r\n:0\r\n", 1));
    free(run(d, r, "SET t v EX 100\r\nTTL t\r\nPERSIST t\r\nTTL t\r\nTTL nothere\r\n",
             "+OK\r\n:100\r\n:1\r\n:-1\r\n:-2\r\n", 1));
    free(run(d, r, "SETEX e 100 v\r\nEXPIRE e 0\r\nGET e\r\nPSETEX p 100000 v\r\n",
             "+OK\r\n:1\r\n$-1\r\n+OK\r\n", 1));
    char *x = run(d, r, "PTTL p\r\n", NULL, 1);
    long long pttl = -5;
    EXPECT(sscanf(x, ":%lld", &pttl) == 1 && pttl > 99000 && pttl <= 100000, "pttl %s", x);
    free(x);
    free(run(d, r, "SETNX n 1\r\nSETNX n 2\r\nGETDEL n\r\nGET n\r\n", ":1\r\n:0\r\n$1\r\n1\r\n$-1\r\n", 1));
    free(run(d, r, "GETEX k EX 50\r\nTTL k\r\n", "$3\r\nnew\r\n:50\r\n", 1));
    run2(d, r, "SET k v EX 0\r\n", "-ERR invalid expire time in 'set' command\r\n");
    run2(d, r, "CONFIG GET append*\r\n",
         "*4\r\n$10\r\nappendonly\r\n$2\r\nno\r\n$11\r\nappendfsync\r\n$8\r\neverysec\r\n");
    /* extreme values: rejected or clamped, never overflowing (found by fuzz_native and code review) */
    run2(d, r, "SET k v EX -9223372036854775808\r\n", "-ERR invalid expire time in 'set' command\r\n");
    run2(d, r, "SET k v PXAT -9223372036854775808\r\n", "-ERR invalid expire time in 'set' command\r\n");
    free(run(d, r, "SET k v\r\nPEXPIREAT k -9223372036854775808\r\nEXISTS k\r\n", "+OK\r\n:1\r\n:0\r\n", 1));
    free(run(d, r, "SET k v\r\nPEXPIRE k 9223372036854775807\r\nEXISTS k\r\n", "+OK\r\n:1\r\n:1\r\n", 1));
    run2(d, r, "SET k v BOGUS\r\n", "-ERR syntax error\r\n");
    run2(d, r, "GET\r\n", "-ERR wrong number of arguments for 'GET' command\r\n");
    run2(d, r, "FOO bar\r\n", "-ERR unknown command 'FOO'\r\n");
    run2(d, r, "SELECT 0\r\nSELECT 1\r\n", "+OK\r\n-ERR DB index is out of range\r\n");
    run2(d, r, "HELLO 4\r\n", "-NOPROTO unsupported protocol version\r\n");
    free(run(d, r, "FLUSHALL\r\nSET k1 v\r\nSET k2 v\r\nSET x v\r\nDBSIZE\r\n",
             "+OK\r\n+OK\r\n+OK\r\n+OK\r\n:3\r\n", 1));
    x = run(d, r, "KEYS k*\r\n", NULL, 1);
    EXPECT(strncmp(x, "*2\r\n", 4) == 0 && strstr(x, "$2\r\nk1\r\n") && strstr(x, "$2\r\nk2\r\n"), "keys: %s",
           x);
    free(x);
    x = run(d, r, "SCAN 0 MATCH x\r\n", NULL, 1);
    EXPECT(strcmp(x, "*2\r\n$1\r\n0\r\n*1\r\n$1\r\nx\r\n") == 0, "scan: %s", x);
    free(x);
    x = run(d, r, "INFO\r\n", NULL, 1);
    EXPECT(x[0] == '$' && strstr(x, "db0:keys=3") != NULL, "info: %.80s", x);
    free(x);
    /* inline and array framing errors */
    struct session s = {.authed = 1};
    x = feed(d, r, &s, "*1\r\n$x\r\n", 8, 0);
    EXPECT(s.close && strncmp(x, "-ERR Protocol error", 19) == 0, "bad bulk length closes: %s", x);
    free(x);
    s = (struct session){.authed = 1};
    x = feed(d, r, &s, "*2\r\n$3\r\nGET\r\n$1\r\nkX\r\n", 21, 0);
    EXPECT(s.close, "bad bulk terminator closes");
    free(x);
    s = (struct session){.authed = 1};
    x = feed(d, r, &s, "QUIT\r\nGET k\r\n", 12, 0);
    EXPECT(s.close && strcmp(x, "+OK\r\n") == 0, "quit: %s", x);
    free(x);
}

static void test_auth(akvd *d) {
    /* d was started with credentials "alice:secret" and the default password "pw". */
    free(run(d, mc_execute, "get a\r\n", "CLIENT_ERROR unauthenticated\r\n", 0));
    free(run(d, mc_execute, "set x 0 0 9\r\nalice bad\r\n", "CLIENT_ERROR authentication failure\r\n", 0));
    free(run(d, mc_execute, "set x 0 0 12\r\nalice secret\r\nget a\r\n", "STORED\r\nEND\r\n", 0));
    free(run(d, resp_execute, "GET a\r\n", "-NOAUTH Authentication required.\r\n", 0));
    free(run(d, resp_execute, "AUTH wrong\r\n",
             "-WRONGPASS invalid username-password pair or user is disabled.\r\n", 0));
    free(run(d, resp_execute, "AUTH pw\r\nGET a\r\n", "+OK\r\n$-1\r\n", 0));
    free(run(d, resp_execute, "AUTH alice secret\r\nGET a\r\n", "+OK\r\n$-1\r\n", 0));
    char *x = run(d, resp_execute, "HELLO 2 AUTH alice secret\r\nGET a\r\n", NULL, 0);
    EXPECT(strncmp(x, "*14\r\n", 5) == 0 && strstr(x, "$-1\r\n") != NULL, "hello with auth: %.40s", x);
    free(x);
}

/* Random inputs: valid commands, single-byte mutations, and noise. */
static void fuzz(akvd *d, exec_fn fn, const char *const *seeds, unsigned nseeds, const char *name) {
    uint64_t st = 0x9e3779b97f4a7c15ull;
    unsigned closes = 0;
    for (unsigned iter = 0; iter < 500000; iter++) {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        char buf[512];
        size_t n = 0;
        unsigned parts = 1 + (unsigned)(st % 4);
        for (unsigned k = 0; k < parts && n < sizeof(buf) - 64; k++) {
            const char *sd = seeds[(st >> (8 + k * 5)) % nseeds];
            size_t l = strlen(sd);
            if (n + l >= sizeof(buf)) {
                break;
            }
            memcpy(buf + n, sd, l);
            n += l;
        }
        unsigned mut = (unsigned)((st >> 40) % 5);
        for (unsigned k = 0; k < mut && n > 0; k++) {
            st ^= st << 13;
            st ^= st >> 7;
            st ^= st << 17;
            buf[st % n] = (char)(st >> 32);
        }
        if ((iter & 7) == 0) {
            n = (size_t)(st % sizeof(buf));
            for (size_t i = 0; i < n; i++) {
                st ^= st << 13;
                st ^= st >> 7;
                st ^= st << 17;
                buf[i] = (char)st;
            }
        }
        struct session s = {.authed = 1};
        struct obuf out = {0};
        size_t used = fn(d, &s, (const uint8_t *)buf, n, &out);
        if (used > n) {
            EXPECT(0, "%s: consumed %zu of %zu bytes", name, used, n);
            break;
        }
        closes += s.close;
        ob_free(&out);
    }
    akv_stats stt;
    akv_stats_get(akvd_store(d), &stt);
    EXPECT(stt.log_bytes <= stt.arena_bytes, "%s: engine consistent after fuzz", name);
    printf("%s fuzz: 500000 inputs, %u closed the connection\n", name, closes);
}

/* Feeds input on an existing session and compares the replies. */
static void sess(akvd *d, exec_fn fn, struct session *ss, const char *in, const char *want) {
    char *got = feed(d, fn, ss, in, strlen(in), 0);
    EXPECT(strcmp(got, want) == 0, "request %.70s\n  got  [%s]\n  want [%s]", in, got, want);
    free(got);
}

/* Returns 1 when the replies to in contain needle. */
static int sess_has(akvd *d, exec_fn fn, struct session *ss, const char *in, const char *needle) {
    char *got = feed(d, fn, ss, in, strlen(in), 0);
    int ok = strstr(got, needle) != NULL;
    EXPECT(ok, "request %.60s: [%s] lacks [%s]", in, got, needle);
    free(got);
    return ok;
}

/* Status of one native request (operation, key, value) on a session. */
static int native(akvd *d, struct session *ss, uint8_t op, const char *key, const void *val, size_t vlen) {
    uint8_t req[512];
    uint8_t resp[4096];
    size_t klen = key != NULL ? strlen(key) : 0;
    struct akvp_req rq = {.op = op, .klen = (uint16_t)klen, .reqid = 7, .vlen = (uint32_t)vlen};
    akvp_req_encode(req, &rq);
    if (klen > 0) {
        memcpy(req + AKVP_REQ_HDR, key, klen);
    }
    if (vlen > 0) {
        memcpy(req + AKVP_REQ_HDR + klen, val, vlen);
    }
    size_t n = akvd_native_execute(d, ss, req, AKVP_REQ_HDR + klen + vlen, resp, sizeof(resp));
    struct akvp_resp rs;
    return n >= AKVP_RESP_HDR && akvp_resp_decode(resp, n, &rs) == 0 ? rs.status : -1;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL && fputs(text, f) >= 0, "write %s", path);
    fclose(f);
}

static void test_resp3_and_transactions(akvd *d) {
    exec_fn r = resp_execute;
    akv_flush(akvd_store(d));
    struct session s3 = {.authed = 1};
    char *hello = feed(d, r, &s3, "HELLO 3\r\n", 9, 0);
    EXPECT(strncmp(hello, "%7\r\n$6\r\nserver\r\n", strlen("%7\r\n$6\r\nserver\r\n")) == 0 &&
               strstr(hello, "$5\r\nproto\r\n:3\r\n") != NULL,
           "HELLO 3 answers a map: %s", hello);
    free(hello);
    sess(d, r, &s3, "GET absent\r\n", "_\r\n");
    sess(d, r, &s3, "CONFIG GET maxmemory-policy\r\n",
         "%1\r\n$16\r\nmaxmemory-policy\r\n$11\r\nallkeys-lru\r\n");
    char *info = feed(d, r, &s3, "INFO\r\n", 6, 0);
    EXPECT(info[0] == '=' && strstr(info, "\r\ntxt:# Server") != NULL, "INFO is a verbatim string in RESP3");
    free(info);
    free(feed(d, r, &s3, "HELLO 2\r\n", 9, 0));
    sess(d, r, &s3, "GET absent\r\n", "$-1\r\n"); /* HELLO 2 switches back (its reply was read above) */
    resp_session_free(&s3);

    free(run(d, r, "MULTI\r\nSET t1 a\r\nINCR c\r\nGET t1\r\nEXEC\r\n",
             "+OK\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n*3\r\n+OK\r\n:1\r\n$1\r\na\r\n", 1));
    free(run(d, r, "MULTI\r\nNOSUCH\r\nSET x 1\r\nEXEC\r\nEXISTS x\r\n",
             "+OK\r\n-ERR unknown command 'NOSUCH'\r\n+QUEUED\r\n-EXECABORT Transaction discarded because of "
             "previous "
             "errors.\r\n:0\r\n",
             1));
    free(run(d, r, "MULTI\r\nSET s abc\r\nINCR s\r\nGET s\r\nEXEC\r\n",
             "+OK\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n*3\r\n+OK\r\n-ERR value is not an integer or out of "
             "range\r\n$3\r\nabc\r\n",
             1));
    run2(d, r, "EXEC\r\nDISCARD\r\n", "-ERR EXEC without MULTI\r\n-ERR DISCARD without MULTI\r\n");
    free(run(d, r, "MULTI\r\nMULTI\r\nWATCH k\r\nDISCARD\r\n",
             "+OK\r\n-ERR MULTI calls can not be nested\r\n-ERR WATCH inside MULTI is not allowed\r\n+OK\r\n",
             1));
    free(run(d, r, "MULTI\r\nSET a 1\r\nDBSIZE\r\nFLUSHALL\r\nDBSIZE\r\nEXEC\r\n",
             "+OK\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n+QUEUED\r\n*4\r\n+OK\r\n:4\r\n+OK\r\n:0\r\n",
             1)); /* t1 c s a */

    /* WATCH: a change by another session between WATCH and EXEC aborts; without one, EXEC runs */
    struct session w1 = {.authed = 1};
    struct session w2 = {.authed = 1};
    sess(d, r, &w1, "SET w v0\r\nWATCH w\r\n", "+OK\r\n+OK\r\n");
    sess(d, r, &w2, "SET w other\r\n", "+OK\r\n");
    sess(d, r, &w1, "MULTI\r\nSET w mine\r\nEXEC\r\nGET w\r\n", "+OK\r\n+QUEUED\r\n*-1\r\n$5\r\nother\r\n");
    sess(d, r, &w1, "WATCH w\r\nMULTI\r\nSET w mine\r\nEXEC\r\nGET w\r\n",
         "+OK\r\n+OK\r\n+QUEUED\r\n*1\r\n+OK\r\n$4\r\nmine\r\n");
    sess(d, r, &w1, "WATCH absent\r\n", "+OK\r\n");
    sess(d, r, &w2, "SET absent now\r\n", "+OK\r\n");
    sess(d, r, &w1, "MULTI\r\nGET absent\r\nEXEC\r\n", "+OK\r\n+QUEUED\r\n*-1\r\n"); /* created since WATCH */
    sess(d, r, &w1, "WATCH w\r\nUNWATCH\r\n", "+OK\r\n+OK\r\n");
    sess(d, r, &w2, "SET w again\r\n", "+OK\r\n");
    sess(d, r, &w1, "MULTI\r\nGET w\r\nEXEC\r\n", "+OK\r\n+QUEUED\r\n*1\r\n$5\r\nagain\r\n");
    free(feed(d, r, &w1, "HELLO 3\r\n", 9, 0));
    sess(d, r, &w1, "WATCH w\r\n", "+OK\r\n");
    sess(d, r, &w2, "DEL w\r\n", ":1\r\n");
    sess(d, r, &w1, "MULTI\r\nGET w\r\nEXEC\r\n",
         "+OK\r\n+QUEUED\r\n_\r\n"); /* the RESP3 null of an aborted EXEC */
    resp_session_free(&w1);
    resp_session_free(&w2);
}

static void test_acl_protocols(akvd *d, const char *path) {
    exec_fn r = resp_execute;
    exec_fn m = mc_execute;
    akv_flush(akvd_store(d));
    struct session app = {0};
    akvd_session_init(d, &app);
    sess(d, r, &app, "GET a\r\n", "-NOAUTH Authentication required.\r\n");
    sess(d, r, &app, "AUTH app wrong\r\n",
         "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
    sess(d, r, &app, "AUTH app apppw\r\n", "+OK\r\n");
    sess(d, r, &app, "SET app:1 v\r\nGET app:1\r\n", "+OK\r\n$1\r\nv\r\n");
    sess(d, r, &app, "SET other v\r\n", "-NOPERM No permissions to access a key\r\n");
    sess(d, r, &app, "GET shared:1\r\n", "$-1\r\n");
    sess(d, r, &app, "SET shared:1 v\r\n", "-NOPERM No permissions to access a key\r\n");
    sess(d, r, &app, "INCR shared:n\r\n", "-NOPERM No permissions to access a key\r\n");
    sess(d, r, &app, "MGET app:1 shared:1\r\n", "*2\r\n$1\r\nv\r\n$-1\r\n");
    sess(d, r, &app, "MGET app:1 other\r\n", "-NOPERM No permissions to access a key\r\n");
    sess(d, r, &app, "MSET app:2 x other y\r\n", "-NOPERM No permissions to access a key\r\n");
    sess(d, r, &app, "FLUSHALL\r\n", "-NOPERM User app has no permissions to run the 'flushall' command\r\n");
    sess(d, r, &app, "ACL LIST\r\n", "-NOPERM User app has no permissions to run the 'acl' command\r\n");
    sess(d, r, &app, "PING\r\n", "+PONG\r\n");
    sess(d, r, &app, "MULTI\r\nSET app:t 1\r\nFLUSHALL\r\nEXEC\r\n",
         "+OK\r\n+QUEUED\r\n-NOPERM User app has no permissions to run the 'flushall' command\r\n-EXECABORT "
         "Transaction "
         "discarded because of previous errors.\r\n");

    struct session adm = {0};
    akvd_session_init(d, &adm);
    sess(d, r, &adm, "AUTH admin adminpw\r\nACL WHOAMI\r\n", "+OK\r\n$5\r\nadmin\r\n");
    sess(d, r, &adm, "ACL USERS\r\n", "*4\r\n$7\r\ndefault\r\n$5\r\nadmin\r\n$3\r\napp\r\n$2\r\nro\r\n");
    sess_has(d, r, &adm, "ACL LIST\r\n", "user app on #");
    sess_has(d, r, &adm, "ACL LIST\r\n", " ~app:* %R~shared:* resetchannels +@all -@dangerous");
    sess_has(d, r, &adm, "ACL CAT\r\n", "$9\r\ndangerous\r\n");
    sess_has(d, r, &adm, "ACL CAT dangerous\r\n", "$8\r\nflushall\r\n");
    sess(d, r, &adm, "ACL DRYRUN app GET app:1\r\n", "+OK\r\n");
    sess(d, r, &adm, "ACL DRYRUN app FLUSHALL\r\n",
         "+User app has no permissions to run the 'flushall' command\r\n");
    sess(d, r, &adm, "ACL DRYRUN app SET other v\r\n",
         "+User app has no permissions to access one of the keys\r\n");
    sess_has(d, r, &adm, "ACL GETUSER ro\r\n", "$5\r\nflags\r\n*1\r\n$2\r\non\r\n");
    sess(d, r, &adm, "ACL GETUSER nobody\r\n", "$-1\r\n");
    sess(d, r, &adm, "ACL SETUSER x on\r\n",
         "-ERR users are defined in the ACL file only; edit it and run ACL LOAD\r\n");

    /* memcached: authentication data "<user> <password>"; read-only user */
    struct session ro = {0};
    akvd_session_init(d, &ro);
    sess(d, m, &ro, "get app:1\r\n", "CLIENT_ERROR unauthenticated\r\n");
    sess(d, m, &ro, "set x 0 0 7\r\nro ropw\r\n", "STORED\r\n");
    sess(d, m, &ro, "get app:1 shared:1\r\n", "VALUE app:1 0 1\r\nv\r\nEND\r\n");
    sess(d, m, &ro, "set app:1 0 0 1\r\nz\r\n", "CLIENT_ERROR permission denied\r\n");
    sess(d, m, &ro, "delete app:1\r\nflush_all\r\n",
         "CLIENT_ERROR permission denied\r\nCLIENT_ERROR permission denied\r\n");
    sess(d, m, &ro, "mg app:1 v\r\n", "VA 1\r\nv\r\n");

    /* native: login, a key outside the patterns, a denied category, MGET per key */
    struct session nat = {0};
    akvd_session_init(d, &nat);
    EXPECT(native(d, &nat, AKVP_GET, "app:1", NULL, 0) == AKVP_NOAUTH, "native: login required");
    EXPECT(native(d, &nat, AKVP_AUTH, NULL, "app:apppw", 9) == AKVP_OK, "native: login");
    EXPECT(native(d, &nat, AKVP_PUT, "app:3", "v", 1) == AKVP_OK, "native: own key");
    EXPECT(native(d, &nat, AKVP_PUT, "other", "v", 1) == AKVP_NOPERM, "native: foreign key");
    EXPECT(native(d, &nat, AKVP_FLUSH, NULL, NULL, 0) == AKVP_NOPERM, "native: dangerous operation");
    uint8_t keys[] = {5, 0, 'a', 'p', 'p', ':', '1', 5, 0, 'o', 't', 'h', 'e', 'r'};
    EXPECT(native(d, &nat, AKVP_MGET, NULL, keys, sizeof(keys)) == AKVP_NOPERM,
           "native: MGET checks every key");
    EXPECT(native(d, &nat, AKVP_MGET, NULL, keys, 7) == AKVP_OK, "native: MGET of permitted keys");

    /* ACL LOAD: a damaged file is refused; then "app" is disabled and its connections are closed */
    write_text(path, "user admin on >adminpw allkeys +@all\nuser app on +@nosuch\n");
    sess_has(d, r, &adm, "ACL LOAD\r\n", "-ERR ");
    sess(d, r, &app, "GET app:1\r\n", "$1\r\nv\r\n");
    write_text(path, "user default off\nuser admin on >adminpw allkeys allchannels +@all\nuser app off\n"
                     "user ro on >ropw ~* -@all +@read +@write\n");
    sess(d, r, &adm, "ACL LOAD\r\n", "+OK\r\n");
    sess(d, r, &app, "GET app:1\r\n", "-ERR the user of this connection was removed or disabled\r\n");
    EXPECT(app.close, "connection of a disabled user is closed");
    EXPECT(native(d, &nat, AKVP_GET, "app:1", NULL, 0) == AKVP_NOAUTH && nat.close,
           "native session closed too");
    sess(d, m, &ro, "set app:1 0 0 1\r\nz\r\n", "STORED\r\n"); /* the new rules of ro apply at once */
}

int main(void) {
    char acl_path[] = "/tmp/akv-test-acl-XXXXXX";
    int afd = mkstemp(acl_path);
    EXPECT(afd >= 0, "ACL file");
    close(afd);
    write_text(acl_path,
               "user default off\nuser admin on >adminpw allkeys allchannels +@all\n"
               "user app on >apppw ~app:* %R~shared:* +@all -@dangerous\nuser ro on >ropw ~* -@all +@read\n");
    char cred_path[] = "/tmp/akv-test-cred-XXXXXX";
    int fd = mkstemp(cred_path);
    const char creds[] = "# test credentials\nalice:secret\npw\n";
    EXPECT(fd >= 0 && write(fd, creds, sizeof(creds) - 1) == (ssize_t)(sizeof(creds) - 1),
           "credentials file");
    close(fd);

    akvd_config cfg;
    akvd_config_default(&cfg);
    cfg.port = NULL; /* no listeners: the protocol modules are driven directly */
    cfg.store.capacity = 8u << 20;
    cfg.store.shards = 4;
    cfg.store.max_entry = 64u << 10;
    cfg.threads = 1;
    cfg.log_level = AKVD_LOG_ERROR;
    int err = 0;
    akvd *d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "server without listeners: %d", err);
    if (d != NULL) {
        test_memcached(d);
        test_resp(d);
        test_resp3_and_transactions(d);
        static const char *const mc_seeds[] = {"get a b\r\n",
                                               "set a 1 0 3\r\nabc\r\n",
                                               "incr a 1\r\n",
                                               "mg a v f c t s k\r\n",
                                               "ms a 2 T5\r\nxy\r\n",
                                               "ma n N0 J1 v\r\n",
                                               "md a\r\n",
                                               "delete a\r\n",
                                               "gat 1 a\r\n",
                                               "cas a 0 0 1 5\r\nq\r\n",
                                               "append a 0 0 1\r\nz\r\n",
                                               "touch a 10\r\n",
                                               "stats\r\n",
                                               "flush_all\r\n",
                                               "set big 0 0 99999\r\n"};
        fuzz(d, mc_execute, mc_seeds, sizeof(mc_seeds) / sizeof(mc_seeds[0]), "memcached");
        static const char *const resp_seeds[] = {"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n",
                                                 "*2\r\n$3\r\nGET\r\n$1\r\na\r\n",
                                                 "INCR a\r\n",
                                                 "MSET a 1 b 2\r\n",
                                                 "MGET a b c\r\n",
                                                 "SET a b EX 5 NX\r\n",
                                                 "EXPIRE a 3\r\n",
                                                 "TTL a\r\n",
                                                 "APPEND a x\r\n",
                                                 "DEL a b\r\n",
                                                 "KEYS *\r\n",
                                                 "*1\r\n$4\r\nPING\r\n",
                                                 "*-1\r\n",
                                                 "$5\r\nhello\r\n",
                                                 "SCAN 0 MATCH a*\r\n"};
        fuzz(d, resp_execute, resp_seeds, sizeof(resp_seeds) / sizeof(resp_seeds[0]), "resp");
        akvd_stop(d);
    }
    cfg.auth_file = cred_path;
    d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "server with credentials: %d", err);
    if (d != NULL) {
        test_auth(d);
        akvd_stop(d);
    }
    cfg.auth_file = NULL;
    cfg.acl_file = acl_path;
    d = akvd_start(&cfg, &err);
    EXPECT(d != NULL, "server with an ACL file: %d", err);
    if (d != NULL) {
        test_acl_protocols(d, acl_path);
        akvd_stop(d);
    }
    unlink(acl_path);
    unlink(cred_path);
    srv_thread_exit(); /* the protocol modules ran on this thread */
    resp_thread_exit();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_proto: ok");
    return 0;
}
