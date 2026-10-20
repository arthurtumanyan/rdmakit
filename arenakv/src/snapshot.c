/*
 * snapshot.c - writing and loading snapshot files (format in srv.h).
 *
 * Writing: akv_foreach() copies one shard at a time under its lock; records are packed into blocks of
 * about BLOCK_TARGET bytes, each protected by a CRC32C. The file is written as "<path>.tmp", flushed with
 * fsync, renamed over <path>, and the directory is synced, so a crash leaves either the old or the new
 * snapshot (rename(2) replaces the target atomically; POSIX.1-2017 rename()).
 *
 * Loading: blocks are verified before their records are stored; a bad magic, a CRC mismatch, a truncated
 * block or a missing end block yields -EBADMSG. Records whose time to live has run out are skipped; other
 * records keep their remaining time to live. CAS versions are not preserved (new versions are assigned).
 */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

#define BLOCK_TARGET (1u << 20)
#define REC_HDR 20u

static const uint8_t FILE_MAGIC[8] = {'A', 'K', 'V', 'S', 'N', 'A', 'P', '1'};
#define BLOCK_MAGIC 0x314b4c42u /* "BLK1" */

/* ---- CRC32C ----------------------------------------------------------------------------------------- */

/* Table for the reflected Castagnoli polynomial 0x82F63B78 (RFC 3720, appendix B.4). */
static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = c & 1 ? (c >> 1) ^ 0x82f63b78u : c >> 1;
        }
        crc_table[i] = c;
    }
}

uint32_t akv_crc32c(uint32_t crc, const void *data, size_t n) {
    pthread_once(&crc_once, crc_init);
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
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

/* ---- writing ---------------------------------------------------------------------------------------- */

struct writer {
    FILE *f;
    struct obuf blk;
    uint32_t count;
    uint64_t total;
    int err;
};

static int write_all(FILE *f, const void *p, size_t n) {
    return fwrite(p, 1, n, f) == n ? 0 : -EIO;
}

static int flush_block(struct writer *w) {
    if (w->count == 0 || w->err != 0) {
        return w->err;
    }
    uint8_t h[16];
    put32(h, BLOCK_MAGIC);
    put32(h + 4, w->count);
    put32(h + 8, (uint32_t)w->blk.len);
    put32(h + 12, akv_crc32c(0, w->blk.p, w->blk.len));
    if (write_all(w->f, h, sizeof(h)) != 0 || write_all(w->f, w->blk.p, w->blk.len) != 0) {
        w->err = -EIO;
    }
    w->blk.len = 0;
    w->count = 0;
    return w->err;
}

static int add_record(void *arg, const akv_record *r) {
    struct writer *w = arg;
    uint8_t h[REC_HDR];
    h[0] = (uint8_t)r->klen;
    h[1] = (uint8_t)(r->klen >> 8);
    h[2] = 0;
    h[3] = 0;
    put32(h + 4, r->flags);
    put32(h + 8, (uint32_t)r->vlen);
    put64(h + 12, (uint64_t)r->ttl_ms);
    if (ob_add(&w->blk, h, sizeof(h)) != 0 || ob_add(&w->blk, r->key, r->klen) != 0 ||
        ob_add(&w->blk, r->val, r->vlen) != 0) {
        w->err = -ENOMEM;
        return w->err;
    }
    w->count++;
    w->total++;
    if (w->blk.len >= BLOCK_TARGET) {
        return flush_block(w);
    }
    return 0;
}

/* Syncs the directory that contains path, so that a rename is durable. */
static void sync_dir(const char *path) {
    char *copy = strdup(path);
    if (copy == NULL) {
        return;
    }
    int fd = open(dirname(copy), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    free(copy);
}

int snap_save(akv *s, const char *path, uint64_t *records) {
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -ENAMETOOLONG;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return -errno;
    }
    struct writer w = {.f = f};
    uint8_t hdr[24];
    memcpy(hdr, FILE_MAGIC, 8);
    put32(hdr + 8, 1);
    put32(hdr + 12, 0);
    put64(hdr + 16, (uint64_t)time(NULL));
    w.err = write_all(f, hdr, sizeof(hdr));
    if (w.err == 0) {
        int rc = akv_foreach(s, add_record, &w);
        if (rc != 0 && w.err == 0) {
            w.err = rc;
        }
    }
    flush_block(&w);
    if (w.err == 0) {
        uint8_t end[24];
        put32(end, BLOCK_MAGIC);
        put32(end + 4, 0);
        put32(end + 8, 8);
        uint8_t total[8];
        put64(total, w.total);
        put32(end + 12, akv_crc32c(0, total, 8));
        memcpy(end + 16, total, 8);
        w.err = write_all(f, end, sizeof(end));
    }
    if (w.err == 0 && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
        w.err = -errno;
    }
    if (fclose(f) != 0 && w.err == 0) {
        w.err = -errno;
    }
    ob_free(&w.blk);
    if (w.err != 0) {
        unlink(tmp);
        return w.err;
    }
    if (rename(tmp, path) != 0) {
        int e = -errno;
        unlink(tmp);
        return e;
    }
    sync_dir(path);
    if (records != NULL) {
        *records = w.total;
    }
    return 0;
}

/* ---- loading ---------------------------------------------------------------------------------------- */

static int read_all(FILE *f, void *p, size_t n) {
    return fread(p, 1, n, f) == n ? 0 : -1;
}

int snap_load(akv *s, const char *path, uint64_t *records) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return errno == ENOENT ? -ENOENT : -errno;
    }
    uint8_t hdr[24];
    if (read_all(f, hdr, sizeof(hdr)) != 0 || memcmp(hdr, FILE_MAGIC, 8) != 0 || get32(hdr + 8) != 1) {
        fclose(f);
        return -EBADMSG;
    }
    uint8_t *buf = NULL;
    size_t cap = 0;
    uint64_t loaded = 0;
    uint64_t seen = 0;
    int rc = -EBADMSG;
    for (;;) {
        uint8_t bh[16];
        if (read_all(f, bh, sizeof(bh)) != 0 || get32(bh) != BLOCK_MAGIC) {
            break; /* truncated or damaged: no end block */
        }
        uint32_t count = get32(bh + 4);
        uint32_t len = get32(bh + 8);
        uint32_t crc = get32(bh + 12);
        if (len > (64u << 20)) {
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
        if (read_all(f, buf, len) != 0 || akv_crc32c(0, buf, len) != crc) {
            break;
        }
        if (count == 0) {
            if (len == 8 && get64(buf) == seen) {
                rc = 0; /* end block: total matches */
            }
            break;
        }
        size_t off = 0;
        uint32_t k = 0;
        for (; k < count && off + REC_HDR <= len; k++) {
            const uint8_t *r = buf + off;
            size_t klen = (size_t)(r[0] | (r[1] << 8));
            uint32_t flags = get32(r + 4);
            size_t vlen = get32(r + 8);
            int64_t ttl = (int64_t)get64(r + 12);
            if (off + REC_HDR + klen + vlen > len || klen == 0) {
                break;
            }
            seen++;
            if (ttl != 0) { /* 0: the time to live ran out while the snapshot was written */
                akv_store_opts o = {.flags = flags, .ttl_ms = ttl < 0 ? 0 : ttl};
                if (akv_store(s, r + REC_HDR, klen, r + REC_HDR + klen, vlen, &o, NULL) == 0) {
                    loaded++;
                }
            }
            off += REC_HDR + klen + vlen;
        }
        if (k != count || off != len) {
            break; /* inconsistent block contents */
        }
    }
    free(buf);
    fclose(f);
    if (records != NULL) {
        *records = loaded;
    }
    return rc;
}
