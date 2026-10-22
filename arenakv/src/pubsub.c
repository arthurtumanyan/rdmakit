/*
 * pubsub.c - publish/subscribe registry of the server, see pubsub.h.
 *
 * Channels live in an open-addressing hash table (linear probing, tombstones on removal, rebuilt when the
 * table is half full); each channel holds the subscribers in an array. Patterns are a plain array of
 * (pattern, subscriber) pairs, matched against the channel of every PUBLISH: deployments have few
 * patterns, and a pattern index would not pay for itself. One read-write lock protects the registry:
 * PUBLISH takes it for reading, (un)subscribing for writing.
 *
 * A PUBLISH formats the message once per protocol version and target and hands it to the inbox of the
 * worker that serves the subscriber (pubsub_deliver_fn). It never touches the subscriber's connection, so
 * publishing threads and workers share no connection state; a message for a connection that closed in the
 * meantime is dropped by the worker, which finds no connection with that id.
 */
#include "pubsub.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acl.h"
#include "hash.h"

struct sub {
    uint64_t conn_id;
    unsigned worker;
    int resp3;
};

struct channel {
    char *name; /* NULL: free slot; TOMBSTONE: removed */
    size_t n;
    uint64_t hash;
    struct sub *subs;
    size_t nsubs;
    size_t cap;
};

struct pattern {
    char *pat;
    size_t n;
    struct sub sub;
};

struct pubsub {
    pthread_rwlock_t lock;
    struct channel *tab;
    size_t tab_cap; /* power of two */
    size_t used;    /* live and removed slots */
    size_t live;
    struct pattern *pats;
    size_t npats;
    size_t pats_cap;
    pubsub_deliver_fn deliver;
    void *arg;
};

static char tombstone;
#define TOMBSTONE (&tombstone)
#define SEED 0x5bd1e9955bd1e995ull

pubsub *pubsub_create(pubsub_deliver_fn deliver, void *arg) {
    pubsub *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->tab_cap = 64;
    p->tab = calloc(p->tab_cap, sizeof(*p->tab));
    if (p->tab == NULL) {
        free(p);
        return NULL;
    }
    pthread_rwlock_init(&p->lock, NULL);
    p->deliver = deliver;
    p->arg = arg;
    return p;
}

void pubsub_destroy(pubsub *p) {
    if (p == NULL) {
        return;
    }
    for (size_t i = 0; i < p->tab_cap; i++) {
        if (p->tab[i].name != NULL && p->tab[i].name != TOMBSTONE) {
            free(p->tab[i].name);
            free(p->tab[i].subs);
        }
    }
    free(p->tab);
    for (size_t i = 0; i < p->npats; i++) {
        free(p->pats[i].pat);
    }
    free(p->pats);
    pthread_rwlock_destroy(&p->lock);
    free(p);
}

/* Slot of channel (name, n): the live slot, or the first free slot of its probe sequence (*found = 0). */
static struct channel *probe(pubsub *p, const char *name, size_t n, uint64_t h, int *found) {
    size_t mask = p->tab_cap - 1;
    struct channel *first_free = NULL;
    for (size_t i = h & mask;; i = (i + 1) & mask) {
        struct channel *c = &p->tab[i];
        if (c->name == NULL) {
            *found = 0;
            return first_free != NULL ? first_free : c;
        }
        if (c->name == TOMBSTONE) {
            if (first_free == NULL) {
                first_free = c;
            }
            continue;
        }
        if (c->hash == h && c->n == n && memcmp(c->name, name, n) == 0) {
            *found = 1;
            return c;
        }
    }
}

/* Rebuilds the table with twice the capacity of the live channels (or the same, to drop tombstones). */
static int rebuild(pubsub *p) {
    size_t cap = p->tab_cap;
    while (p->live * 4 >= cap) {
        cap *= 2;
    }
    struct channel *tab = calloc(cap, sizeof(*tab));
    if (tab == NULL) {
        return -ENOMEM;
    }
    struct channel *old = p->tab;
    size_t old_cap = p->tab_cap;
    p->tab = tab;
    p->tab_cap = cap;
    p->used = p->live;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].name != NULL && old[i].name != TOMBSTONE) {
            int found;
            *probe(p, old[i].name, old[i].n, old[i].hash, &found) = old[i];
        }
    }
    free(old);
    return 0;
}

int pubsub_subscribe(pubsub *p, const char *name, size_t n, uint64_t conn_id, unsigned worker, int resp3) {
    uint64_t h = akv_hash64(name, n, SEED);
    pthread_rwlock_wrlock(&p->lock);
    int rc = 0;
    if ((p->used + 1) * 2 > p->tab_cap) {
        rc = rebuild(p);
    }
    int found = 0;
    struct channel *c = rc == 0 ? probe(p, name, n, h, &found) : NULL;
    if (c != NULL && !found) {
        char *copy = malloc(n + 1);
        if (copy == NULL) {
            rc = -ENOMEM;
        } else {
            memcpy(copy, name, n);
            copy[n] = '\0';
            if (c->name == NULL) {
                p->used++;
            }
            *c = (struct channel){.name = copy, .n = n, .hash = h};
            p->live++;
        }
    }
    if (rc == 0 && c == NULL) {
        rc = -ENOMEM; /* probe() always finds a slot after rebuild(); kept for the analyzer */
    }
    if (rc == 0) {
        for (size_t i = 0; i < c->nsubs; i++) {
            if (c->subs[i].conn_id == conn_id) {
                rc = 1; /* already subscribed */
            }
        }
    }
    if (rc == 0 && c->nsubs == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 4;
        struct sub *s = realloc(c->subs, cap * sizeof(*s));
        if (s == NULL) {
            rc = -ENOMEM;
        } else {
            c->subs = s;
            c->cap = cap;
        }
    }
    if (rc == 0) {
        c->subs[c->nsubs++] = (struct sub){.conn_id = conn_id, .worker = worker, .resp3 = resp3};
    }
    pthread_rwlock_unlock(&p->lock);
    return rc < 0 ? rc : rc == 0;
}

int pubsub_unsubscribe(pubsub *p, const char *name, size_t n, uint64_t conn_id) {
    uint64_t h = akv_hash64(name, n, SEED);
    pthread_rwlock_wrlock(&p->lock);
    int found;
    struct channel *c = probe(p, name, n, h, &found);
    int removed = 0;
    if (found) {
        for (size_t i = 0; i < c->nsubs; i++) {
            if (c->subs[i].conn_id == conn_id) {
                c->subs[i] = c->subs[--c->nsubs];
                removed = 1;
                break;
            }
        }
        if (c->nsubs == 0) {
            free(c->name);
            free(c->subs);
            *c = (struct channel){.name = TOMBSTONE};
            p->live--;
        }
    }
    pthread_rwlock_unlock(&p->lock);
    return removed;
}

int pubsub_psubscribe(pubsub *p, const char *pat, size_t n, uint64_t conn_id, unsigned worker, int resp3) {
    pthread_rwlock_wrlock(&p->lock);
    int rc = 1;
    for (size_t i = 0; i < p->npats; i++) {
        if (p->pats[i].sub.conn_id == conn_id && p->pats[i].n == n && memcmp(p->pats[i].pat, pat, n) == 0) {
            rc = 0;
        }
    }
    if (rc == 1 && p->npats == p->pats_cap) {
        size_t cap = p->pats_cap ? p->pats_cap * 2 : 8;
        struct pattern *np = realloc(p->pats, cap * sizeof(*np));
        if (np == NULL) {
            rc = -ENOMEM;
        } else {
            p->pats = np;
            p->pats_cap = cap;
        }
    }
    if (rc == 1) {
        char *copy = malloc(n + 1);
        if (copy == NULL) {
            rc = -ENOMEM;
        } else {
            memcpy(copy, pat, n);
            copy[n] = '\0';
            p->pats[p->npats++] = (struct pattern){
                .pat = copy, .n = n, .sub = {.conn_id = conn_id, .worker = worker, .resp3 = resp3}};
        }
    }
    pthread_rwlock_unlock(&p->lock);
    return rc;
}

int pubsub_punsubscribe(pubsub *p, const char *pat, size_t n, uint64_t conn_id) {
    pthread_rwlock_wrlock(&p->lock);
    int removed = 0;
    for (size_t i = 0; i < p->npats; i++) {
        if (p->pats[i].sub.conn_id == conn_id && p->pats[i].n == n && memcmp(p->pats[i].pat, pat, n) == 0) {
            free(p->pats[i].pat);
            p->pats[i] = p->pats[--p->npats];
            removed = 1;
            break;
        }
    }
    pthread_rwlock_unlock(&p->lock);
    return removed;
}

/* Appends a RESP bulk string. */
static int bulk(char *out, size_t cap, size_t *len, const char *p, size_t n) {
    int w = snprintf(out + *len, cap - *len, "$%zu\r\n", n);
    if (w < 0 || (size_t)w + n + 2 > cap - *len) {
        return -1;
    }
    *len += (size_t)w;
    memcpy(out + *len, p, n);
    *len += n;
    memcpy(out + *len, "\r\n", 2);
    *len += 2;
    return 0;
}

/*
 * Formats a pub/sub message ("message" channel payload, or "pmessage" pattern channel payload) as a RESP2
 * array or a RESP3 push into a new buffer. Returns the length, or 0 when memory is exhausted.
 */
static size_t format(int resp3, const char *pat, size_t pn, const char *ch, size_t cn, const char *msg,
                     size_t mn, char **out) {
    size_t cap = 64 + pn + cn + mn;
    char *b = malloc(cap);
    if (b == NULL) {
        return 0;
    }
    size_t len = (size_t)snprintf(b, cap, "%c%d\r\n", resp3 ? '>' : '*', pat != NULL ? 4 : 3);
    int bad = pat != NULL ? bulk(b, cap, &len, "pmessage", 8) || bulk(b, cap, &len, pat, pn)
                          : bulk(b, cap, &len, "message", 7);
    bad = bad || bulk(b, cap, &len, ch, cn) || bulk(b, cap, &len, msg, mn);
    if (bad) {
        free(b);
        return 0;
    }
    *out = b;
    return len;
}

size_t pubsub_publish(pubsub *p, const char *ch, size_t cn, const char *msg, size_t mn) {
    uint64_t h = akv_hash64(ch, cn, SEED);
    size_t receivers = 0;
    char *plain[2] = {NULL, NULL}; /* RESP2 and RESP3 "message" frames, shared by channel subscribers */
    size_t plain_len[2] = {0, 0};
    pthread_rwlock_rdlock(&p->lock);
    int found;
    struct channel *c = probe(p, ch, cn, h, &found);
    for (size_t i = 0; found && i < c->nsubs; i++) {
        int v = c->subs[i].resp3 != 0;
        if (plain[v] == NULL) {
            plain_len[v] = format(v, NULL, 0, ch, cn, msg, mn, &plain[v]);
        }
        if (plain_len[v] > 0 &&
            p->deliver(p->arg, c->subs[i].worker, c->subs[i].conn_id, plain[v], plain_len[v]) == 0) {
            receivers++;
        }
    }
    for (size_t i = 0; i < p->npats; i++) {
        if (!acl_glob(p->pats[i].pat, p->pats[i].n, ch, cn)) {
            continue;
        }
        char *f = NULL;
        size_t n = format(p->pats[i].sub.resp3, p->pats[i].pat, p->pats[i].n, ch, cn, msg, mn, &f);
        if (n > 0 && p->deliver(p->arg, p->pats[i].sub.worker, p->pats[i].sub.conn_id, f, n) == 0) {
            receivers++;
        }
        free(f);
    }
    pthread_rwlock_unlock(&p->lock);
    free(plain[0]);
    free(plain[1]);
    return receivers;
}

size_t pubsub_channels(pubsub *p, const char *pat, size_t pn, int (*fn)(void *arg, const char *ch, size_t n),
                       void *arg) {
    size_t count = 0;
    pthread_rwlock_rdlock(&p->lock);
    for (size_t i = 0; i < p->tab_cap; i++) {
        struct channel *c = &p->tab[i];
        if (c->name == NULL || c->name == TOMBSTONE) {
            continue;
        }
        if (pat == NULL || acl_glob(pat, pn, c->name, c->n)) {
            count++;
            if (fn != NULL) {
                fn(arg, c->name, c->n);
            }
        }
    }
    pthread_rwlock_unlock(&p->lock);
    return count;
}

size_t pubsub_numsub(pubsub *p, const char *ch, size_t n) {
    uint64_t h = akv_hash64(ch, n, SEED);
    pthread_rwlock_rdlock(&p->lock);
    int found;
    struct channel *c = probe(p, ch, n, h, &found);
    size_t r = found ? c->nsubs : 0;
    pthread_rwlock_unlock(&p->lock);
    return r;
}

size_t pubsub_numpat(pubsub *p) {
    pthread_rwlock_rdlock(&p->lock);
    size_t unique = 0;
    for (size_t i = 0; i < p->npats; i++) {
        int seen = 0;
        for (size_t j = 0; j < i && !seen; j++) {
            seen = p->pats[j].n == p->pats[i].n && memcmp(p->pats[j].pat, p->pats[i].pat, p->pats[i].n) == 0;
        }
        unique += !seen;
    }
    pthread_rwlock_unlock(&p->lock);
    return unique;
}
