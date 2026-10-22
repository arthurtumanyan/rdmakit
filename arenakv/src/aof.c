/*
 * aof.c - append-only log, see aof.h.
 *
 * Locks: `m` protects the append buffer, the sequence counter and the writer's control flags; `mlock`
 * protects the manifest state (file numbers). aof_append() runs with an engine shard lock held and takes
 * `m` only; the writer and the rewrite thread never call the engine while holding either lock, so there
 * is no lock-order cycle with the shard locks.
 */
#include "aof.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

#define FILE_HDR 24u
#define BLK_HDR 16u
#define REC_HDR 28u
#define BLOCK_TARGET (4u << 20)
#define BLOCK_MAX (64u << 20)
#define PENDING_MAX (256ull << 20)
#define BLOCK_MAGIC 0x31474c41u /* "ALG1" */
#define KIND_BASE 0u
#define KIND_INCR 1u

static const char FILE_MAGIC[8] = {'A', 'K', 'V', 'L', 'O', 'G', '0', '1'};

struct aof {
    aof_config cfg;
    akv *s;
    char *dir;
    pthread_mutex_t m;
    pthread_cond_t cv;      /* wakes the writer */
    pthread_cond_t space;   /* wakes appenders waiting for buffer space */
    pthread_cond_t rotated; /* wakes a rewrite waiting for the switch to a new increment file */
    struct obuf cur;        /* records appended and not yet taken by the writer */
    struct obuf spare;
    uint64_t seq;
    int writer_idle;
    int stop;
    int rotate_req;
    uint64_t rotate_gen;
    int rotate_ok;
    pthread_t writer;
    int writer_started;
    int fd; /* current increment file, owned by the writer thread */
    uint64_t fd_size;
    /* manifest state */
    pthread_mutex_t mlock;
    uint64_t base_n;
    uint64_t *incr;
    size_t nincr;
    uint64_t next_n;
    /* rewrite */
    pthread_mutex_t rlock;
    pthread_t rewriter;
    int rewriter_joinable;
    atomic_int rewrite_running;
    atomic_int abort_rewrite;
    /* state */
    atomic_ullong durable;
    atomic_ullong base_size;
    atomic_ullong current_size;
    atomic_ullong appended;
    atomic_ullong fsyncs;
    atomic_ullong rewrites;
    atomic_int last_write_ok;
    atomic_int last_rewrite_ok;
};

static _Thread_local uint64_t thread_seq;

/* ---- helpers ------------------------------------------------------------------------------------------ */

static void aof_logf(aof *a, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void aof_logf(aof *a, int level, const char *fmt, ...) {
    if (a->cfg.log == NULL) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    a->cfg.log(a->cfg.log_arg, level, msg);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void file_path(const aof *a, char *out, size_t n, const char *kind, uint64_t num) {
    snprintf(out, n, "%s/%s-%llu.aof", a->dir, kind, (unsigned long long)num);
}

/* Writes all n bytes of the iovecs. Returns 0 or -errno. */
static int write_iov(int fd, struct iovec *iov, int cnt) {
    while (cnt > 0) {
        ssize_t w = writev(fd, iov, cnt);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        while (cnt > 0 && (size_t)w >= iov->iov_len) {
            w -= (ssize_t)iov->iov_len;
            iov++;
            cnt--;
        }
        if (cnt > 0) {
            iov->iov_base = (uint8_t *)iov->iov_base + w;
            iov->iov_len -= (size_t)w;
        }
    }
    return 0;
}

static int write_all(int fd, const void *p, size_t n) {
    struct iovec v = {.iov_base = (void *)p, .iov_len = n};
    return write_iov(fd, &v, 1);
}

static void sync_dir(const aof *a) {
    int fd = open(a->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static int write_file_header(int fd, uint32_t kind) {
    uint8_t h[FILE_HDR];
    memcpy(h, FILE_MAGIC, 8);
    put32(h + 8, 1);
    put32(h + 12, kind);
    put64(h + 16, (uint64_t)time(NULL));
    return write_all(fd, h, sizeof(h));
}

/* Size of the record starting at p (at least REC_HDR bytes available). */
static size_t rec_size(const uint8_t *p) {
    return REC_HDR + (size_t)(p[2] | (p[3] << 8)) + get32(p + 8);
}

/* Appends a record to b. Returns 0 or -ENOMEM. */
static int encode(struct obuf *b, unsigned kind, unsigned reason, const void *key, size_t klen,
                  const void *val, size_t vlen, uint32_t flags, int64_t expire, uint64_t txn) {
    uint8_t *p = ob_reserve(b, REC_HDR + klen + vlen);
    if (p == NULL) {
        return -ENOMEM;
    }
    p[0] = (uint8_t)kind;
    p[1] = (uint8_t)reason;
    p[2] = (uint8_t)klen;
    p[3] = (uint8_t)(klen >> 8);
    put32(p + 4, flags);
    put32(p + 8, (uint32_t)vlen);
    put64(p + 12, (uint64_t)expire);
    put64(p + 20, txn);
    if (klen > 0) {
        memcpy(p + REC_HDR, key, klen);
    }
    if (vlen > 0) {
        memcpy(p + REC_HDR + klen, val, vlen);
    }
    ob_commit(b, REC_HDR + klen + vlen);
    return 0;
}

/* Writes the records in p[0, n) as blocks of about BLOCK_TARGET bytes. Returns 0 or -errno. */
static int write_blocks(int fd, const uint8_t *p, size_t n, uint64_t *written) {
    size_t off = 0;
    while (off < n) {
        size_t start = off;
        uint32_t count = 0;
        while (off < n && (off == start || off - start + rec_size(p + off) <= BLOCK_TARGET)) {
            off += rec_size(p + off);
            count++;
        }
        uint8_t h[BLK_HDR];
        put32(h, BLOCK_MAGIC);
        put32(h + 4, count);
        put32(h + 8, (uint32_t)(off - start));
        put32(h + 12, akv_crc32c(0, p + start, off - start));
        struct iovec v[2] = {{.iov_base = h, .iov_len = BLK_HDR},
                             {.iov_base = (void *)(p + start), .iov_len = off - start}};
        int rc = write_iov(fd, v, 2);
        if (rc != 0) {
            return rc;
        }
        *written += BLK_HDR + (off - start);
    }
    return 0;
}

/* ---- manifest ----------------------------------------------------------------------------------------- */

/* Replaces the manifest with the current state (mlock held). Returns 0 or -errno. */
static int manifest_write(aof *a) {
    char tmp[4096];
    char path[4096];
    snprintf(tmp, sizeof(tmp), "%s/manifest.tmp", a->dir);
    snprintf(path, sizeof(path), "%s/manifest", a->dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    char text[256];
    int n = snprintf(text, sizeof(text), "akv-aof 1\nbase base-%llu.aof\n", (unsigned long long)a->base_n);
    int rc = write_all(fd, text, (size_t)n);
    for (size_t i = 0; i < a->nincr && rc == 0; i++) {
        n = snprintf(text, sizeof(text), "incr incr-%llu.aof\n", (unsigned long long)a->incr[i]);
        rc = write_all(fd, text, (size_t)n);
    }
    if (rc == 0 && fsync(fd) != 0) {
        rc = -errno;
    }
    close(fd);
    if (rc == 0 && rename(tmp, path) != 0) {
        rc = -errno;
    }
    if (rc != 0) {
        unlink(tmp);
        return rc;
    }
    sync_dir(a);
    return 0;
}

/* Reads the manifest. Returns 0, -ENOENT (none) or -EBADMSG. */
static int manifest_read(aof *a) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/manifest", a->dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return errno == ENOENT ? -ENOENT : -errno;
    }
    char line[256];
    int rc = 0;
    int have_header = 0;
    int have_base = 0;
    while (rc == 0 && fgets(line, sizeof(line), f) != NULL) {
        unsigned long long n;
        if (!have_header) {
            have_header = strcmp(line, "akv-aof 1\n") == 0;
            rc = have_header ? 0 : -EBADMSG;
        } else if (sscanf(line, "base base-%llu.aof", &n) == 1 && !have_base) {
            a->base_n = n;
            have_base = 1;
        } else if (sscanf(line, "incr incr-%llu.aof", &n) == 1) {
            uint64_t *p = realloc(a->incr, (a->nincr + 1) * sizeof(*p));
            if (p == NULL) {
                rc = -ENOMEM;
                break;
            }
            a->incr = p;
            a->incr[a->nincr++] = n;
        } else {
            rc = -EBADMSG;
        }
    }
    fclose(f);
    if (rc == 0 && (!have_base || a->nincr == 0)) {
        rc = -EBADMSG;
    }
    a->next_n = a->base_n + 1;
    for (size_t i = 0; i < a->nincr; i++) {
        if (a->incr[i] + 1 > a->next_n) {
            a->next_n = a->incr[i] + 1;
        }
    }
    return rc;
}

/* Opens a new increment file numbered n for appending, with its header. Returns the descriptor or -errno. */
static int incr_create(aof *a, uint64_t n) {
    char path[4096];
    file_path(a, path, sizeof(path), "incr", n);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    int rc = write_file_header(fd, KIND_INCR);
    if (rc == 0 && fdatasync(fd) != 0) {
        rc = -errno;
    }
    if (rc != 0) {
        close(fd);
        unlink(path);
        return rc;
    }
    return fd;
}

static uint64_t file_size(const aof *a, const char *kind, uint64_t n) {
    char path[4096];
    file_path(a, path, sizeof(path), kind, n);
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

/* Recomputes the size counters from the files of the manifest. */
static void update_sizes(aof *a) {
    pthread_mutex_lock(&a->mlock);
    uint64_t base = file_size(a, "base", a->base_n);
    uint64_t total = base;
    for (size_t i = 0; i < a->nincr; i++) {
        total += file_size(a, "incr", a->incr[i]);
    }
    pthread_mutex_unlock(&a->mlock);
    atomic_store(&a->base_size, base);
    atomic_store(&a->current_size, total);
}

/* ---- base files --------------------------------------------------------------------------------------- */

struct base_writer {
    aof *a;
    int fd;
    struct obuf blk;
    int64_t now;
    uint64_t bytes;
    int err;
};

static int base_add(void *arg, const akv_record *r) {
    struct base_writer *b = arg;
    if (atomic_load(&b->a->abort_rewrite)) {
        return -ECANCELED;
    }
    if (r->ttl_ms == 0) {
        return 0; /* expires now */
    }
    int64_t expire = r->ttl_ms < 0 ? 0 : b->now + r->ttl_ms;
    if (encode(&b->blk, AKV_CHG_PUT, 0, r->key, r->klen, r->val, r->vlen, r->flags, expire, 0) != 0) {
        return -ENOMEM;
    }
    if (b->blk.len >= BLOCK_TARGET) {
        int rc = write_blocks(b->fd, b->blk.p, b->blk.len, &b->bytes);
        b->blk.len = 0;
        return rc;
    }
    return 0;
}

/* Writes base file n from the engine's content (temporary name, sync, rename). Returns 0 or -errno. */
static int base_write(aof *a, uint64_t n) {
    char path[4096];
    char tmp[4200];
    file_path(a, path, sizeof(path), "base", n);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    struct base_writer b = {.a = a, .fd = fd, .now = wall_ms(), .bytes = FILE_HDR};
    int rc = write_file_header(fd, KIND_BASE);
    if (rc == 0) {
        rc = akv_foreach(a->s, base_add, &b);
    }
    if (rc == 0 && b.blk.len > 0) {
        rc = write_blocks(fd, b.blk.p, b.blk.len, &b.bytes);
    }
    if (rc == 0 && fsync(fd) != 0) {
        rc = -errno;
    }
    close(fd);
    ob_free(&b.blk);
    if (rc == 0 && rename(tmp, path) != 0) {
        rc = -errno;
    }
    if (rc != 0) {
        unlink(tmp);
        return rc;
    }
    sync_dir(a);
    return 0;
}

/* ---- loading ------------------------------------------------------------------------------------------ */

struct pending_txn {
    uint64_t id;
    struct obuf recs;
};

struct loader {
    aof *a;
    struct pending_txn *t;
    size_t nt;
    aof_load_info *info;
};

static void decode(const uint8_t *p, akv_change *c) {
    size_t klen = (size_t)(p[2] | (p[3] << 8));
    *c = (akv_change){
        .kind = (enum akv_change_kind)p[0],
        .reason = (enum akv_del_reason)p[1],
        .key = klen > 0 ? p + REC_HDR : NULL,
        .klen = klen,
        .flags = get32(p + 4),
        .vlen = get32(p + 8),
        .expire_unix_ms = (int64_t)get64(p + 12),
        .txn = get64(p + 20),
    };
    c->val = c->vlen > 0 ? p + REC_HDR + klen : NULL;
}

/* Applies one record or holds it back for its transaction. Returns 0, -EBADMSG or -ENOMEM. */
static int load_rec(struct loader *L, const uint8_t *p) {
    akv_change c;
    decode(p, &c);
    if (c.kind == AOF_TXN_END) {
        for (size_t i = 0; i < L->nt; i++) {
            if (L->t[i].id != c.txn) {
                continue;
            }
            struct obuf *b = &L->t[i].recs;
            for (size_t off = 0; off < b->len; off += rec_size(b->p + off)) {
                akv_change r;
                decode(b->p + off, &r);
                akv_apply(L->a->s, &r);
                L->info->records++;
            }
            ob_free(b);
            L->t[i] = L->t[--L->nt];
            break;
        }
        return 0;
    }
    if (c.kind < AKV_CHG_PUT || c.kind > AKV_CHG_FLUSH || (c.kind != AKV_CHG_FLUSH && c.klen == 0)) {
        return -EBADMSG;
    }
    if (c.txn == 0) {
        akv_apply(L->a->s, &c);
        L->info->records++;
        return 0;
    }
    size_t i = 0;
    while (i < L->nt && L->t[i].id != c.txn) {
        i++;
    }
    if (i == L->nt) {
        struct pending_txn *t = realloc(L->t, (L->nt + 1) * sizeof(*t));
        if (t == NULL) {
            return -ENOMEM;
        }
        L->t = t;
        L->t[L->nt++] = (struct pending_txn){.id = c.txn};
    }
    return ob_add(&L->t[i].recs, p, rec_size(p));
}

/* 1 when bytes [off, size) of fd are all zero (a tail the file system extended but never wrote). */
static int zero_tail(int fd, uint64_t off, uint64_t size) {
    uint8_t buf[65536];
    while (off < size) {
        size_t n = size - off < sizeof(buf) ? (size_t)(size - off) : sizeof(buf);
        if (pread(fd, buf, n, (off_t)off) != (ssize_t)n) {
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != 0) {
                return 0;
            }
        }
        off += n;
    }
    return 1;
}

/*
 * Loads one file. last: it is the last increment file, so a torn end is cut off. Returns 0, or -EBADMSG
 * for damage (*damage_at holds the offset of the damaged block), or -errno.
 */
static int load_file(struct loader *L, const char *path, int last, uint64_t *damage_at) {
    int fd = open(path, (last ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    uint64_t size = (uint64_t)st.st_size;
    uint8_t h[FILE_HDR];
    int rc = 0;
    uint64_t off = FILE_HDR;
    uint64_t torn_at = UINT64_MAX;
    if (size < FILE_HDR) {
        if (last) {
            torn_at = 0; /* crash while the file was created: rewrite its header below */
        } else {
            rc = -EBADMSG;
            *damage_at = 0;
        }
        off = size;
    } else if (pread(fd, h, FILE_HDR, 0) != (ssize_t)FILE_HDR || memcmp(h, FILE_MAGIC, 8) != 0 ||
               get32(h + 8) != 1) {
        rc = -EBADMSG;
        *damage_at = 0;
    }
    uint8_t *buf = NULL;
    size_t cap = 0;
    while (rc == 0 && off < size && torn_at == UINT64_MAX) {
        uint8_t bh[BLK_HDR];
        int short_hdr = size - off < BLK_HDR;
        if (!short_hdr && pread(fd, bh, BLK_HDR, (off_t)off) != (ssize_t)BLK_HDR) {
            rc = -errno;
            break;
        }
        uint32_t len = short_hdr ? 0 : get32(bh + 8);
        int bad_hdr = !short_hdr && (get32(bh) != BLOCK_MAGIC || len > BLOCK_MAX);
        int short_body = !short_hdr && !bad_hdr && off + BLK_HDR + len > size;
        if (short_hdr || short_body || (bad_hdr && zero_tail(fd, off, size))) {
            if (last) {
                torn_at = off;
            } else {
                rc = -EBADMSG;
                *damage_at = off;
            }
            break;
        }
        if (bad_hdr) {
            rc = -EBADMSG;
            *damage_at = off;
            break;
        }
        if (len > cap) {
            uint8_t *p = realloc(buf, len);
            if (p == NULL) {
                rc = -ENOMEM;
                break;
            }
            buf = p;
            cap = len;
        }
        if (pread(fd, buf, len, (off_t)(off + BLK_HDR)) != (ssize_t)len) {
            rc = -EIO;
            break;
        }
        if (akv_crc32c(0, buf, len) != get32(bh + 12)) {
            if (last && (off + BLK_HDR + len == size || zero_tail(fd, off + BLK_HDR + len, size))) {
                torn_at = off;
            } else {
                rc = -EBADMSG;
                *damage_at = off;
            }
            break;
        }
        uint32_t count = get32(bh + 4);
        size_t p = 0;
        uint32_t k = 0;
        while (k < count && p + REC_HDR <= len && p + rec_size(buf + p) <= len) {
            rc = load_rec(L, buf + p);
            if (rc != 0) {
                break;
            }
            p += rec_size(buf + p);
            k++;
        }
        if (rc == 0 && (k != count || p != len)) {
            rc = -EBADMSG;
        }
        if (rc == -EBADMSG) {
            *damage_at = off;
        }
        off += BLK_HDR + len;
    }
    free(buf);
    if (rc == 0 && torn_at != UINT64_MAX) {
        L->info->truncated += size - torn_at;
        if (ftruncate(fd, (off_t)torn_at) != 0) {
            rc = -errno;
        } else if (torn_at == 0) {
            rc = write_file_header(fd, KIND_INCR);
        }
        if (rc == 0 && fsync(fd) != 0) {
            rc = -errno;
        }
    }
    close(fd);
    L->info->files++;
    return rc;
}

/* Loads the files of the manifest into the engine. Returns 0 or a negative error. */
static int load_all(aof *a, aof_load_info *info) {
    struct loader L = {.a = a, .info = info};
    char path[4096];
    uint64_t damage_at = 0;
    file_path(a, path, sizeof(path), "base", a->base_n);
    int rc = load_file(&L, path, 0, &damage_at);
    for (size_t i = 0; i < a->nincr && rc == 0; i++) {
        file_path(a, path, sizeof(path), "incr", a->incr[i]);
        rc = load_file(&L, path, i + 1 == a->nincr, &damage_at);
    }
    if (rc == -EBADMSG) {
        aof_logf(a, 0, "append-only log: damaged block at offset %llu of %s", (unsigned long long)damage_at,
                 path);
        if (a->cfg.load_damaged) {
            info->damaged = 1;
            rc = 0;
        }
    }
    for (size_t i = 0; i < L.nt; i++) {
        struct obuf *b = &L.t[i].recs;
        for (size_t off = 0; off < b->len; off += rec_size(b->p + off)) {
            info->dropped_txn++;
        }
        ob_free(b);
    }
    free(L.t);
    return rc;
}

/* ---- writer ------------------------------------------------------------------------------------------- */

static void sync_fd(aof *a) {
    if (fdatasync(a->fd) == 0) {
        atomic_fetch_add(&a->fsyncs, 1);
    } else {
        atomic_store(&a->last_write_ok, 0);
        aof_logf(a, 0, "append-only log: fdatasync failed: %s", strerror(errno));
    }
}

/* Switches to a new increment file and records it in the manifest (writer thread). */
static void rotate(aof *a) {
    sync_fd(a);
    pthread_mutex_lock(&a->mlock);
    uint64_t n = a->next_n++;
    int fd = incr_create(a, n);
    int ok = 0;
    if (fd >= 0) {
        uint64_t *p = realloc(a->incr, (a->nincr + 1) * sizeof(*p));
        if (p != NULL) {
            a->incr = p;
            a->incr[a->nincr++] = n;
            if (manifest_write(a) == 0) {
                ok = 1;
            } else {
                a->nincr--;
            }
        }
        if (!ok) {
            char path[4096];
            file_path(a, path, sizeof(path), "incr", n);
            close(fd);
            unlink(path);
        }
    }
    pthread_mutex_unlock(&a->mlock);
    if (ok) {
        close(a->fd);
        a->fd = fd;
        a->fd_size = FILE_HDR;
    } else {
        aof_logf(a, 0, "append-only log: cannot switch to a new increment file");
    }
    pthread_mutex_lock(&a->m);
    a->rotate_ok = ok;
    a->rotate_gen++;
    pthread_cond_broadcast(&a->rotated);
    pthread_mutex_unlock(&a->m);
}

static void maybe_auto_rewrite(aof *a) {
    if (a->cfg.rewrite_percentage == 0 || atomic_load(&a->rewrite_running)) {
        return;
    }
    uint64_t base = atomic_load(&a->base_size);
    uint64_t cur = atomic_load(&a->current_size);
    if (cur >= a->cfg.rewrite_min_size && cur - base >= base / 100 * a->cfg.rewrite_percentage) {
        if (aof_rewrite(a) == 0) {
            aof_logf(a, 2, "append-only log: automatic rewrite started (%llu bytes, base %llu)",
                     (unsigned long long)cur, (unsigned long long)base);
        }
    }
}

static void *writer_main(void *arg) {
    aof *a = arg;
    struct timespec last_sync;
    clock_gettime(CLOCK_MONOTONIC, &last_sync);
    int dirty = 0;
    pthread_mutex_lock(&a->m);
    for (;;) {
        while (a->cur.len == 0 && !a->stop && !a->rotate_req) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (dirty && a->cfg.fsync == AOF_FSYNC_EVERYSEC && now.tv_sec > last_sync.tv_sec) {
                break; /* one second passed with unsynced data */
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 200000000;
            if (until.tv_nsec >= 1000000000) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000;
            }
            a->writer_idle = 1;
            pthread_cond_timedwait(&a->cv, &a->m, &until);
            a->writer_idle = 0;
        }
        struct obuf batch = a->cur;
        a->cur = a->spare;
        a->spare = (struct obuf){0};
        uint64_t batch_seq = a->seq;
        int rotate_now = a->rotate_req;
        a->rotate_req = 0;
        int stopping = a->stop;
        pthread_cond_broadcast(&a->space);
        pthread_mutex_unlock(&a->m);

        int written_ok = 1;
        if (batch.len > 0) {
            for (;;) {
                uint64_t written = 0;
                int rc = write_blocks(a->fd, batch.p, batch.len, &written);
                if (rc == 0) {
                    a->fd_size += written;
                    atomic_fetch_add(&a->current_size, written);
                    if (!atomic_load(&a->last_write_ok)) {
                        aof_logf(a, 2, "append-only log: writing again");
                    }
                    atomic_store(&a->last_write_ok, 1);
                    break;
                }
                /* Cut a partly written block off so that the file stays a sequence of whole blocks. */
                if (ftruncate(a->fd, (off_t)a->fd_size) != 0) {
                    /* the next successful write follows the partial block; loading then stops there */
                }
                if (atomic_exchange(&a->last_write_ok, 0)) {
                    aof_logf(a, 0, "append-only log: write failed (%s); retrying every second",
                             strerror(-rc));
                }
                struct timespec ts = {.tv_sec = 1};
                nanosleep(&ts, NULL);
                if (stopping) {
                    written_ok = 0;
                    break;
                }
            }
            dirty = 1;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (dirty && (a->cfg.fsync == AOF_FSYNC_ALWAYS || stopping || rotate_now ||
                      (a->cfg.fsync == AOF_FSYNC_EVERYSEC && now.tv_sec > last_sync.tv_sec))) {
            sync_fd(a);
            last_sync = now;
            dirty = 0;
        }
        /* With AOF_FSYNC_ALWAYS the batch has been synced above; otherwise "durable" means written. */
        if (written_ok && batch_seq != atomic_load(&a->durable)) {
            atomic_store(&a->durable, batch_seq);
            if (a->cfg.durable != NULL) {
                a->cfg.durable(a->cfg.durable_arg);
            }
        }
        batch.len = 0;
        batch.off = 0;
        pthread_mutex_lock(&a->m);
        if (a->spare.p == NULL) {
            a->spare = batch; /* keep the allocation for the next swap */
        } else {
            ob_free(&batch);
        }
        pthread_mutex_unlock(&a->m);
        if (rotate_now) {
            rotate(a);
        }
        if (batch_seq != 0) {
            maybe_auto_rewrite(a);
        }
        pthread_mutex_lock(&a->m);
        if (stopping && a->cur.len == 0) {
            break;
        }
    }
    pthread_mutex_unlock(&a->m);
    return NULL;
}

/* ---- rewrite ------------------------------------------------------------------------------------------ */

static void *rewrite_main(void *arg) {
    aof *a = arg;
    pthread_mutex_lock(&a->m);
    uint64_t gen = a->rotate_gen;
    a->rotate_req = 1;
    pthread_cond_signal(&a->cv);
    while (a->rotate_gen == gen && !a->stop) {
        pthread_cond_wait(&a->rotated, &a->m);
    }
    int ok = a->rotate_gen != gen && a->rotate_ok;
    pthread_mutex_unlock(&a->m);
    int rc = -ECANCELED;
    uint64_t n = 0;
    if (ok) {
        pthread_mutex_lock(&a->mlock);
        n = a->next_n++;
        pthread_mutex_unlock(&a->mlock);
        rc = base_write(a, n);
    }
    if (rc == 0) {
        pthread_mutex_lock(&a->mlock);
        uint64_t old_base = a->base_n;
        size_t old_n = a->nincr;
        uint64_t *old = malloc(old_n * sizeof(*old));
        if (old == NULL) {
            rc = -ENOMEM;
        } else {
            memcpy(old, a->incr, old_n * sizeof(*old));
            a->base_n = n;
            a->incr[0] = old[old_n - 1]; /* the increment file opened by the switch */
            a->nincr = 1;
            rc = manifest_write(a);
            if (rc != 0) {
                /* The previous manifest stays in place; the new base file is not referenced. */
                a->base_n = old_base;
                memcpy(a->incr, old, old_n * sizeof(*old));
                a->nincr = old_n;
            }
        }
        pthread_mutex_unlock(&a->mlock);
        char path[4096];
        if (rc == 0) {
            file_path(a, path, sizeof(path), "base", old_base);
            unlink(path);
            for (size_t i = 0; i + 1 < old_n; i++) {
                file_path(a, path, sizeof(path), "incr", old[i]);
                unlink(path);
            }
            sync_dir(a);
        } else {
            file_path(a, path, sizeof(path), "base", n);
            unlink(path);
        }
        free(old);
    }
    update_sizes(a);
    if (rc == 0) {
        atomic_fetch_add(&a->rewrites, 1);
        aof_logf(a, 2, "append-only log: rewrite done, base %llu bytes",
                 (unsigned long long)atomic_load(&a->base_size));
    } else if (rc != -ECANCELED) {
        aof_logf(a, 0, "append-only log: rewrite failed: %s", strerror(-rc));
    }
    atomic_store(&a->last_rewrite_ok, rc == 0 || rc == -ECANCELED);
    atomic_store(&a->rewrite_running, 0);
    return NULL;
}

int aof_rewrite(aof *a) {
    pthread_mutex_lock(&a->rlock);
    if (atomic_load(&a->rewrite_running)) {
        pthread_mutex_unlock(&a->rlock);
        return -EBUSY;
    }
    if (a->rewriter_joinable) {
        pthread_join(a->rewriter, NULL);
        a->rewriter_joinable = 0;
    }
    atomic_store(&a->rewrite_running, 1);
    int rc = pthread_create(&a->rewriter, NULL, rewrite_main, a) == 0 ? 0 : -EAGAIN;
    if (rc == 0) {
        a->rewriter_joinable = 1;
    } else {
        atomic_store(&a->rewrite_running, 0);
    }
    pthread_mutex_unlock(&a->rlock);
    return rc;
}

/* ---- public ------------------------------------------------------------------------------------------- */

void aof_append(aof *a, const akv_change *c) {
    pthread_mutex_lock(&a->m);
    while (a->cur.len > PENDING_MAX && !a->stop) {
        pthread_cond_wait(&a->space, &a->m);
    }
    if (encode(&a->cur, c->kind, c->reason, c->key, c->klen, c->val, c->vlen, c->flags, c->expire_unix_ms,
               c->txn) != 0) {
        atomic_store(&a->last_write_ok, 0); /* the log misses this change; INFO reports the error */
    }
    thread_seq = ++a->seq;
    if (a->writer_idle) {
        pthread_cond_signal(&a->cv);
    }
    pthread_mutex_unlock(&a->m);
    atomic_fetch_add(&a->appended, 1);
}

void aof_append_txn_end(aof *a, uint64_t txn) {
    akv_change c = {.kind = (enum akv_change_kind)AOF_TXN_END, .txn = txn};
    aof_append(a, &c);
}

uint64_t aof_thread_seq(void) {
    return thread_seq;
}

uint64_t aof_durable(const aof *a) {
    return atomic_load(&((aof *)a)->durable);
}

void aof_info_get(aof *a, aof_info *out) {
    pthread_mutex_lock(&a->m);
    out->pending = a->cur.len;
    pthread_mutex_unlock(&a->m);
    out->base_size = atomic_load(&a->base_size);
    out->current_size = atomic_load(&a->current_size);
    out->appended = atomic_load(&a->appended);
    out->fsyncs = atomic_load(&a->fsyncs);
    out->rewrites = atomic_load(&a->rewrites);
    out->rewrite_running = atomic_load(&a->rewrite_running);
    out->last_rewrite_ok = atomic_load(&a->last_rewrite_ok);
    out->last_write_ok = atomic_load(&a->last_write_ok);
}

static void aof_free(aof *a) {
    if (a->fd >= 0) {
        close(a->fd);
    }
    ob_free(&a->cur);
    ob_free(&a->spare);
    free(a->incr);
    free(a->dir);
    pthread_mutex_destroy(&a->m);
    pthread_mutex_destroy(&a->mlock);
    pthread_mutex_destroy(&a->rlock);
    pthread_cond_destroy(&a->cv);
    pthread_cond_destroy(&a->space);
    pthread_cond_destroy(&a->rotated);
    free(a);
}

/* 1 when dir holds files named like log files. */
static int has_log_files(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        found = strncmp(e->d_name, "base-", 5) == 0 || strncmp(e->d_name, "incr-", 5) == 0;
    }
    closedir(d);
    return found;
}

int aof_open(const aof_config *cfg, akv *s, int load, aof **out, aof_load_info *info) {
    memset(info, 0, sizeof(*info));
    aof *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return -ENOMEM;
    }
    a->cfg = *cfg;
    a->s = s;
    a->fd = -1;
    a->dir = strdup(cfg->dir);
    pthread_mutex_init(&a->m, NULL);
    pthread_mutex_init(&a->mlock, NULL);
    pthread_mutex_init(&a->rlock, NULL);
    pthread_cond_init(&a->cv, NULL);
    pthread_cond_init(&a->space, NULL);
    pthread_cond_init(&a->rotated, NULL);
    atomic_store(&a->last_write_ok, 1);
    atomic_store(&a->last_rewrite_ok, 1);
    if (a->dir == NULL) {
        aof_free(a);
        return -ENOMEM;
    }
    if (mkdir(a->dir, 0700) != 0 && errno != EEXIST) {
        int rc = -errno;
        aof_free(a);
        return rc;
    }
    int rc = manifest_read(a);
    int rewrite_now = 0;
    if (rc == -ENOENT) {
        if (has_log_files(a->dir)) {
            aof_logf(a, 0, "append-only log: %s holds log files but no manifest", a->dir);
            aof_free(a);
            return -EBADMSG;
        }
        /* New log: the current content becomes the first base. */
        a->base_n = 1;
        a->next_n = 3;
        rc = base_write(a, 1);
        if (rc == 0) {
            rc = incr_create(a, 2);
            if (rc >= 0) {
                a->fd = rc;
                rc = 0;
                a->incr = malloc(sizeof(*a->incr));
                if (a->incr == NULL) {
                    rc = -ENOMEM;
                } else {
                    a->incr[0] = 2;
                    a->nincr = 1;
                    rc = manifest_write(a);
                }
            }
        }
        info->created = 1;
    } else if (rc == 0) {
        if (load) {
            rc = load_all(a, info);
            rewrite_now = info->damaged;
        } else {
            rewrite_now = 1;
        }
        if (rc == 0) {
            char path[4096];
            file_path(a, path, sizeof(path), "incr", a->incr[a->nincr - 1]);
            a->fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
            rc = a->fd >= 0 ? 0 : -errno;
        }
    }
    if (rc != 0) {
        aof_free(a);
        return rc;
    }
    struct stat st;
    a->fd_size = fstat(a->fd, &st) == 0 ? (uint64_t)st.st_size : 0;
    update_sizes(a);
    if (pthread_create(&a->writer, NULL, writer_main, a) != 0) {
        aof_free(a);
        return -EAGAIN;
    }
    a->writer_started = 1;
    if (rewrite_now) {
        aof_rewrite(a);
    }
    *out = a;
    return 0;
}

int aof_close(aof *a) {
    atomic_store(&a->abort_rewrite, 1);
    /* A rewrite waiting for the switch is released by the stop flag; it is joined after the writer. */
    pthread_mutex_lock(&a->m);
    a->stop = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_cond_broadcast(&a->space);
    pthread_cond_broadcast(&a->rotated);
    pthread_mutex_unlock(&a->m);
    if (a->writer_started) {
        pthread_join(a->writer, NULL);
    }
    pthread_mutex_lock(&a->rlock);
    if (a->rewriter_joinable) {
        pthread_join(a->rewriter, NULL);
        a->rewriter_joinable = 0;
    }
    pthread_mutex_unlock(&a->rlock);
    int rc = atomic_load(&a->last_write_ok) ? 0 : -EIO;
    aof_free(a);
    return rc;
}
