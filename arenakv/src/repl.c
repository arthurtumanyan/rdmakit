/*
 * repl.c - replication backlog and record stream parser, see repl.h.
 *
 * The backlog is a ring of `size` bytes; byte `off` of the stream is stored at off % size. The ring holds
 * the stream bytes [first, end): appending past the capacity advances first, so a replica that is more than
 * `size` bytes behind can no longer continue partially. One mutex protects the ring; a condition variable
 * wakes the senders of the replicas when bytes are appended. Records are encoded before the lock is taken.
 */
#include "repl.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "aof.h"
#include "srv.h"

struct repl_backlog {
    pthread_mutex_t m;
    pthread_cond_t cv;
    unsigned deep_waiters; /* readers in the long wait of repl_backlog_read(): appends wake them */
    uint8_t *ring;
    size_t size;
    uint64_t first;
    uint64_t end;
    char id[REPL_ID_LEN];
    char id2[REPL_ID_LEN];
    uint64_t second_off; /* id2 is valid for offsets up to this one; 0 = no second id */
};

/*
 * Readers that have caught up first wait SHALLOW_WAITS times SHALLOW_MS without being woken, so that a stream of
 * appends costs at most one reader wake-up per SHALLOW_MS instead of one per record (each a context switch when
 * the worker and the feed thread share a CPU); only then do they wait until an append wakes them. An idle
 * replica therefore receives the first change at once and a busy one within SHALLOW_MS.
 */
#define SHALLOW_WAITS 2
#define SHALLOW_MS 1

static _Thread_local uint64_t thread_offset;
/* Encoding buffer of the calling thread. A thread-specific key frees it when the thread exits. */
static _Thread_local struct obuf scratch;
static _Thread_local int scratch_registered;
static pthread_key_t scratch_key;
static pthread_once_t scratch_once = PTHREAD_ONCE_INIT;

static void scratch_free(void *p) {
    ob_free(p);
}

static void scratch_key_create(void) {
    pthread_key_create(&scratch_key, scratch_free);
}

static void new_id(char id[REPL_ID_LEN]) {
    uint8_t raw[REPL_ID_LEN / 2];
    if (getrandom(raw, sizeof(raw), 0) != (ssize_t)sizeof(raw)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        for (size_t i = 0; i < sizeof(raw); i++) {
            raw[i] = (uint8_t)((uint64_t)ts.tv_nsec * (i + 1) >> 7);
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        id[2 * i] = hex[raw[i] >> 4];
        id[2 * i + 1] = hex[raw[i] & 15];
    }
}

repl_backlog *repl_backlog_create(size_t size) {
    repl_backlog *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->ring = malloc(size);
    if (b->ring == NULL) {
        free(b);
        return NULL;
    }
    b->size = size;
    b->first = b->end = 1;
    new_id(b->id);
    memset(b->id2, '0', REPL_ID_LEN);
    pthread_mutex_init(&b->m, NULL);
    pthread_cond_init(&b->cv, NULL);
    return b;
}

void repl_backlog_destroy(repl_backlog *b) {
    if (b == NULL) {
        return;
    }
    pthread_mutex_destroy(&b->m);
    pthread_cond_destroy(&b->cv);
    free(b->ring);
    free(b);
}

/* Copies n bytes into the ring at the end (lock held). */
static void ring_put(repl_backlog *b, const uint8_t *p, size_t n) {
    if (n >= b->size) {
        p += n - b->size; /* only the last `size` bytes can be held */
        b->end += n - b->size;
        n = b->size;
    }
    size_t pos = (size_t)(b->end % b->size);
    size_t first_part = n < b->size - pos ? n : b->size - pos;
    memcpy(b->ring + pos, p, first_part);
    memcpy(b->ring, p + first_part, n - first_part);
    b->end += n;
    if (b->end - b->first > b->size) {
        b->first = b->end - b->size;
    }
}

uint64_t repl_backlog_append(repl_backlog *b, const akv_change *c) {
    if (!scratch_registered) {
        pthread_once(&scratch_once, scratch_key_create);
        pthread_setspecific(scratch_key, &scratch);
        scratch_registered = 1;
    }
    scratch.len = scratch.off = 0;
    if (aof_record_encode(&scratch, c) != 0) {
        return 0;
    }
    pthread_mutex_lock(&b->m);
    ring_put(b, scratch.p, scratch.len);
    uint64_t end = b->end;
    if (b->deep_waiters > 0) {
        pthread_cond_broadcast(&b->cv);
    }
    pthread_mutex_unlock(&b->m);
    if (scratch.cap > (1u << 20)) {
        ob_free(&scratch); /* keep the thread's buffer small after a large value */
    }
    thread_offset = end;
    return end;
}

void repl_backlog_append_raw(repl_backlog *b, const uint8_t *p, size_t n) {
    pthread_mutex_lock(&b->m);
    ring_put(b, p, n);
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->m);
}

uint64_t repl_backlog_end(repl_backlog *b) {
    pthread_mutex_lock(&b->m);
    uint64_t e = b->end;
    pthread_mutex_unlock(&b->m);
    return e;
}

uint64_t repl_thread_offset(void) {
    return thread_offset;
}

/* Absolute CLOCK_REALTIME time ms milliseconds from now, for pthread_cond_timedwait(). */
static struct timespec after_ms(int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    return t;
}

long repl_backlog_read(repl_backlog *b, uint64_t off, uint8_t *out, size_t cap, int timeout_ms) {
    pthread_mutex_lock(&b->m);
    if (off == b->end && timeout_ms > 0) {
        struct timespec until = after_ms(timeout_ms);
        for (int i = 0; i < SHALLOW_WAITS && off == b->end; i++) {
            struct timespec t = after_ms(SHALLOW_MS);
            pthread_cond_timedwait(&b->cv, &b->m, &t); /* not woken by appends */
        }
        b->deep_waiters++;
        while (off == b->end) {
            if (pthread_cond_timedwait(&b->cv, &b->m, &until) != 0) {
                break;
            }
        }
        b->deep_waiters--;
    }
    long rc;
    if (off < b->first || off > b->end) {
        rc = -ERANGE;
    } else {
        size_t n = (size_t)(b->end - off) < cap ? (size_t)(b->end - off) : cap;
        size_t pos = (size_t)(off % b->size);
        size_t first_part = n < b->size - pos ? n : b->size - pos;
        memcpy(out, b->ring + pos, first_part);
        memcpy(out + first_part, b->ring, n - first_part);
        rc = (long)n;
    }
    pthread_mutex_unlock(&b->m);
    return rc;
}

int repl_backlog_plan(repl_backlog *b, const char id[REPL_ID_LEN], uint64_t off, uint64_t *start) {
    pthread_mutex_lock(&b->m);
    int in_range = off >= b->first && off <= b->end;
    int partial =
        in_range && (memcmp(id, b->id, REPL_ID_LEN) == 0 ||
                     (b->second_off != 0 && memcmp(id, b->id2, REPL_ID_LEN) == 0 && off <= b->second_off));
    *start = partial ? off : b->end;
    pthread_mutex_unlock(&b->m);
    return partial;
}

void repl_backlog_ids(repl_backlog *b, char id[REPL_ID_LEN], char id2[REPL_ID_LEN], uint64_t *second_off) {
    pthread_mutex_lock(&b->m);
    memcpy(id, b->id, REPL_ID_LEN);
    memcpy(id2, b->id2, REPL_ID_LEN);
    *second_off = b->second_off;
    pthread_mutex_unlock(&b->m);
}

void repl_backlog_reset(repl_backlog *b, const char id[REPL_ID_LEN], uint64_t off) {
    pthread_mutex_lock(&b->m);
    memcpy(b->id, id, REPL_ID_LEN);
    memset(b->id2, '0', REPL_ID_LEN);
    b->second_off = 0;
    b->first = b->end = off;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->m);
}

void repl_backlog_switch_id(repl_backlog *b, const char id[REPL_ID_LEN]) {
    pthread_mutex_lock(&b->m);
    if (memcmp(id, b->id, REPL_ID_LEN) != 0) {
        memcpy(b->id2, b->id, REPL_ID_LEN);
        b->second_off = b->end;
        memcpy(b->id, id, REPL_ID_LEN);
    }
    pthread_mutex_unlock(&b->m);
}

void repl_backlog_promote(repl_backlog *b) {
    pthread_mutex_lock(&b->m);
    memcpy(b->id2, b->id, REPL_ID_LEN);
    b->second_off = b->end;
    new_id(b->id);
    pthread_mutex_unlock(&b->m);
}

void repl_backlog_range(repl_backlog *b, uint64_t *first, uint64_t *held) {
    pthread_mutex_lock(&b->m);
    *first = b->first;
    *held = b->end - b->first;
    pthread_mutex_unlock(&b->m);
}

/* ---- stream parser ------------------------------------------------------------------------------------ */

/* Largest record the parser accepts: a damaged length must not make it buffer without bound. */
#define REPL_RECORD_MAX (1ull << 30)

int repl_parse(repl_parser *p, const uint8_t *data, size_t n, int (*fn)(void *arg, const akv_change *c),
               void *arg) {
    if (n == 0) {
        return 0;
    }
    if (p->len + n > p->cap) {
        size_t cap = p->cap ? p->cap : 65536;
        while (cap < p->len + n) {
            cap *= 2;
        }
        uint8_t *nb = realloc(p->buf, cap);
        if (nb == NULL) {
            return -ENOMEM;
        }
        p->buf = nb;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;
    size_t off = 0;
    int rc = 0;
    while (rc == 0 && p->len - off >= AOF_REC_HDR) {
        size_t sz = aof_record_size(p->buf + off);
        if (sz > REPL_RECORD_MAX) {
            rc = -EBADMSG;
            break;
        }
        if (p->len - off < sz) {
            break;
        }
        akv_change c;
        if (aof_record_decode(p->buf + off, &c) != 0) {
            rc = -EBADMSG;
            break;
        }
        rc = fn(arg, &c);
        off += sz;
    }
    memmove(p->buf, p->buf + off, p->len - off);
    p->len -= off;
    return rc;
}

void repl_parser_free(repl_parser *p) {
    free(p->buf);
    memset(p, 0, sizeof(*p));
}
