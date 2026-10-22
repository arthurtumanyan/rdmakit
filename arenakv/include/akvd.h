/*
 * akvd.h - key-value server: serves an akv engine over the native protocol (akvproto.h, carried by
 * rdmaduct over RDMA or TCP), the memcached text and meta protocols, and RESP2 (Redis protocol).
 *
 * Threads: one accept thread waits on all listeners and hands each new connection to one of `threads`
 * worker threads (round robin). A worker serves all its connections from one epoll loop: native
 * connections through rdmaduct's pollable descriptors, text protocols on non-blocking sockets. Requests
 * of a connection are executed in order and may be pipelined. A background thread runs periodic
 * snapshots. The server owns its engine and transport context.
 *
 * Persistence: snapshots (snapshot_path), an append-only log of every change (aof_dir, see src/aof.h) and
 * warm restart (store.shm_path: the cache memory survives a restart of the process). At start the warm
 * memory is used when it is valid, otherwise the append-only log, otherwise the snapshot.
 */
#ifndef AKVD_H
#define AKVD_H

#include <stdint.h>

#include "akv.h"
#include "rdmaduct.h"

typedef struct akvd akvd;

/* Log levels of akvd_log(). */
enum akvd_log_level {
    AKVD_LOG_ERROR = 0,
    AKVD_LOG_WARN = 1,
    AKVD_LOG_INFO = 2,
    AKVD_LOG_DEBUG = 3,
};

typedef struct akvd_config {
    akv_config store;
    /* Native protocol listener (rdmaduct). port NULL disables it. */
    enum duct_transport transport;
    enum duct_wait wait;
    const char *addr;    /* listen address of the native listener; NULL = all IPv4 addresses */
    const char *port;    /* native port; "0" selects a free port */
    uint32_t max_msg;    /* largest native request/response message; 0 selects 64 KiB */
    uint32_t recv_depth; /* receive buffers per native connection; 0 selects 16 */
    uint32_t send_depth; /* send buffers per native connection; 0 selects 16 */
    /* Text protocol listeners on plain TCP sockets. NULL disables a listener; "0" selects a free port. */
    const char *text_addr;      /* listen address of the text listeners; NULL = all IPv4 addresses */
    const char *memcached_port; /* memcached text and meta protocol */
    const char *resp_port;      /* RESP2 */
    /* TLS listeners (only when built with TLS=1); certificate and key files in PEM format. */
    const char *tls_memcached_port;
    const char *tls_resp_port;
    const char *tls_cert;
    const char *tls_key;
    const char *tls_ca;      /* when set, clients must present a certificate signed by this CA */
    unsigned threads;        /* worker threads; 0 selects the CPUs of the affinity mask */
    unsigned max_conns;      /* connections above this number are closed after accept; 0 = unlimited */
    unsigned idle_timeout_s; /* close connections idle for this many seconds; 0 = never */
    /*
     * Credentials file: one entry per line, "user:password" or a bare password (user "default"); empty
     * lines and lines starting with '#' are ignored. When set, every protocol requires authentication
     * before other commands. NULL disables authentication.
     */
    const char *auth_file;
    /*
     * Access control list (src/acl.h: a subset of the Valkey ACL file syntax) with users, passwords and
     * per-user permissions; excludes auth_file. Reloaded by ACL LOAD. NULL disables access control.
     */
    const char *acl_file;
    /* Snapshot file; NULL disables persistence. Loaded at start when it exists. */
    const char *snapshot_path;
    unsigned save_interval_s; /* periodic snapshot when entries changed; 0 = only on request and exit */
    int save_on_stop;         /* write a snapshot in akvd_stop() */
    /* Append-only log directory; NULL disables the log. */
    const char *aof_dir;
    /*
     * fsync policy of the log, as Valkey's appendfsync: 0 no (the kernel decides), 1 everysec (default;
     * up to about one second of acknowledged changes can be lost in a crash), 2 always (a reply that
     * follows a change is sent only after the change is on disk).
     */
    unsigned aof_fsync;
    unsigned aof_rewrite_percentage; /* rewrite when the log grew by this percentage of its base; 0 = off */
    uint64_t aof_rewrite_min_size;   /* ... and is at least this many bytes (default 64 MiB) */
    int aof_load_damaged; /* start with the records before a damaged block (then rewrite) instead of failing */
    /* Pending replies of a pub/sub subscriber above which it is disconnected; 0 selects 32 MiB, the
     * pubsub client-output-buffer-limit of Valkey. */
    uint64_t pubsub_output_limit;
    /*
     * Replication: replicaof_host and replicaof_port name the native listener of a primary to follow
     * (NULL: none); primary_auth is "user:password" (or a password) for it; replica_read_only refuses
     * client writes while following (default 1); repl_backlog_size bounds how far a replica may fall
     * behind and still continue (0 selects 64 MiB).
     */
    /*
     * Cluster protocol (src/cluster.h): the slot map file shared by all nodes and this node's id. A replica
     * in the map follows its primary unless replicaof_host is set. NULL disables cluster mode.
     */
    const char *cluster_config;
    const char *cluster_node_id;
    const char *replicaof_host;
    const char *replicaof_port;
    const char *primary_auth;
    int replica_read_only;
    size_t repl_backlog_size;
    enum akvd_log_level log_level;
    const char *log_path; /* NULL = standard error */
} akvd_config;

/* Fills cfg with defaults: 256 MiB store, AUTO transport, native port 7600, text listeners off, log policy
 * everysec with automatic rewrites at 100% growth above 64 MiB. */
void akvd_config_default(akvd_config *cfg);

/*
 * Creates the engine, loads the snapshot (if configured and present), starts the listeners and threads.
 * Returns NULL on failure with a negative error code in *err (if not NULL): rdmaduct or -errno codes,
 * -EINVAL for an invalid configuration, -EBADMSG for a corrupt snapshot, -ENOTSUP for TLS options in a
 * build without TLS.
 */
akvd *akvd_start(const akvd_config *cfg, int *err);

/* Port of the native listener (0 when disabled). */
int akvd_port(const akvd *d);

/* Port of the memcached and RESP listeners (0 when disabled). */
int akvd_memcached_port(const akvd *d);
int akvd_resp_port(const akvd *d);
int akvd_tls_memcached_port(const akvd *d);
int akvd_tls_resp_port(const akvd *d);

/* Engine of the server. */
akv *akvd_store(akvd *d);

/* Number of connections currently served. */
unsigned akvd_connections(const akvd *d);

/* Writes a snapshot now (synchronously). Returns 0, -EINVAL when persistence is not configured, or -errno. */
int akvd_save(akvd *d);

/* Starts a snapshot on a background thread. Returns 0, -EBUSY (one is running) or -EINVAL (not configured). */
int akvd_bgsave(akvd *d);

/* 1 while a background snapshot runs. */
int akvd_bgsave_running(const akvd *d);

/* Starts a rewrite of the append-only log. Returns 0, -EBUSY (one is running) or -EINVAL (no log). */
int akvd_aof_rewrite(akvd *d);

/* Unix time of the last successful snapshot, 0 when none was written since the start. */
uint64_t akvd_lastsave(const akvd *d);

/* 1 when the last snapshot attempt succeeded (or none was attempted), 0 after a failure. */
int akvd_last_save_ok(const akvd *d);

/* Server version, as reported by INFO, stats and version ("major.minor.patch"). */
const char *akvd_version(void);

/* Reopens the log file (for log rotation). */
void akvd_reopen_log(akvd *d);

/* Writes a line to the server log when level is at or below the configured level. */
void akvd_log(akvd *d, enum akvd_log_level level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/*
 * Stops accepting, closes all connections, joins the threads, writes and syncs the append-only log,
 * saves the snapshot if configured, keeps the cache memory for a warm restart if configured
 * (akv_detach), and frees everything.
 */
void akvd_stop(akvd *d);

#endif
