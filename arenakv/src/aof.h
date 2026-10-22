/*
 * aof.h - append-only log of the engine's change feed (internal interface of the server).
 *
 * The log lives in a directory:
 *   manifest        text file naming the current base file and the increment files in order
 *   base-N.aof      one PUT record per live entry, written by the rewrite that created it
 *   incr-N.aof      change records appended after the switch to this file
 * Every file starts with a 24-byte header (magic "AKVLOG01", version, file kind, creation Unix time) and
 * continues with blocks: 4 bytes magic "ALG1", 4 bytes record count, 4 bytes payload length, 4 bytes
 * CRC32C of the payload, then the records. A record is kind (1), reason (1), key length (2), flags (4),
 * value length (4), absolute expiry in Unix milliseconds (8, 0 = none), transaction id (8), key, value.
 * Records describe resulting states (akv_change), so applying a record twice has no further effect; this
 * is what makes a rewrite that overlaps with new appends correct.
 *
 * Writing: a writer thread takes the records appended since its last pass, writes them as blocks and
 * syncs the file according to the fsync policy (the policies of Valkey's appendfsync: always, everysec,
 * no). The durable sequence number then tells which records are on disk.
 *
 * Rewriting: the writer switches to a new increment file and records it in the manifest; a background
 * thread writes a new base from the engine's current content; the manifest is then replaced by one that
 * names the new base and the new increment file, and the older files are removed. Each manifest is
 * written to a temporary file, synced and renamed, so a crash at any point leaves a manifest whose files
 * replay to the latest state.
 *
 * Loading: the base and the increment files are applied with akv_apply() in manifest order. Records of a
 * transaction (non-zero id) are held back until its end record (AOF_TXN_END) and an incomplete transaction
 * at the end of the log is dropped. A torn last block of the last increment file (the remains of a crash
 * during a write) is cut off and the file truncated, as Valkey does with aof-load-truncated yes; damage
 * anywhere else stops the start unless load_damaged is set.
 */
#ifndef AKV_AOF_H
#define AKV_AOF_H

#include <stdint.h>

#include "akv.h"

/* fsync policies. */
enum aof_fsync {
    AOF_FSYNC_NO = 0,       /* the kernel decides when data reaches the disk */
    AOF_FSYNC_EVERYSEC = 1, /* fdatasync about once per second: up to about one second of changes lost */
    AOF_FSYNC_ALWAYS = 2,   /* fdatasync after every write; replies wait for it (group commit) */
};

/* Record kind of a transaction end; written by the server while it holds the transaction's lock set. */
#define AOF_TXN_END 100

typedef struct aof aof;

typedef struct aof_config {
    const char *dir;
    enum aof_fsync fsync;
    unsigned rewrite_percentage; /* rewrite when the log grew by this percentage of the base; 0 = never */
    uint64_t rewrite_min_size;   /* ... and is at least this large (bytes) */
    int load_damaged; /* on damage before the end: load the records before it, then rewrite (else refuse) */
    /* Log sink (level: 0 error, 1 warning, 2 information). */
    void (*log)(void *arg, int level, const char *msg);
    void *log_arg;
    /* Called by the writer thread after the durable sequence number advanced. */
    void (*durable)(void *arg);
    void *durable_arg;
} aof_config;

/* Outcome of aof_open(). */
typedef struct aof_load_info {
    int created;          /* 1 when the directory held no log and a base was written from the engine */
    uint64_t records;     /* records applied */
    uint64_t files;       /* files read */
    uint64_t truncated;   /* bytes of a torn last block cut off */
    uint64_t dropped_txn; /* records of an incomplete transaction at the end that were not applied */
    int damaged;          /* a damaged block was met (load_damaged): records after it were not applied */
} aof_load_info;

/* State for INFO persistence. */
typedef struct aof_info {
    uint64_t base_size;    /* bytes of the current base file */
    uint64_t current_size; /* bytes of the base and all increment files */
    uint64_t appended;     /* records appended since aof_open() */
    uint64_t pending;      /* bytes appended but not yet written */
    uint64_t fsyncs;
    uint64_t rewrites; /* completed rewrites */
    int rewrite_running;
    int last_rewrite_ok;
    int last_write_ok;
} aof_info;

/*
 * Opens the log in cfg->dir (created with mode 0700 when missing) and starts the writer thread.
 *   load = 1: applies the log to s (which must hold nothing the log does not describe).
 *   load = 0: s already holds the latest state (warm restart): the files are not applied, and a rewrite
 *             from s starts at once, so that the log matches s even if it was not written to the end.
 * A directory without a manifest gets a new base written from the current content of s.
 * Returns 0, -EBADMSG (damaged log, or log files without a manifest), -ENOMEM, or -errno.
 */
int aof_open(const aof_config *cfg, akv *s, int load, aof **out, aof_load_info *info);

/* Appends a change record. Called from the change feed (shard lock held); never blocks on the disk,
 * but waits while more than 256 MiB are pending (the writer cannot keep up). */
void aof_append(aof *a, const akv_change *c);

/* Appends the end record of transaction txn. */
void aof_append_txn_end(aof *a, uint64_t txn);

/* Sequence number of the last record appended by the calling thread (0 = none yet). */
uint64_t aof_thread_seq(void);

/* Highest sequence number written (and, with AOF_FSYNC_ALWAYS, synced). */
uint64_t aof_durable(const aof *a);

/* Starts a rewrite in the background. Returns 0 or -EBUSY when one runs. */
int aof_rewrite(aof *a);

void aof_info_get(aof *a, aof_info *out);

/* Writes and syncs everything appended, stops the threads (an unfinished rewrite is abandoned) and
 * frees the log. Returns 0, or a negative error when the last write failed. */
int aof_close(aof *a);

#endif
