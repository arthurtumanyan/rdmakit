/*
 * sentinel.h - automatic failover of a replicated arenakvd group by a set of monitors (akv_sentinel).
 *
 * A group is one primary and its replicas, all listed in every monitor's configuration (RESP port for
 * monitoring and commands, native port for REPLICAOF). Several monitors watch the same group and know
 * each other. The procedure follows Redis Sentinel (https://redis.io/docs/latest/operate/oss_and_stack/
 * management/sentinel/):
 *
 *   1. Every monitor pings the primary (PING over RESP). A primary without a valid reply for down_after_ms
 *      is subjectively down (s_down) for that monitor.
 *   2. A monitor that sees the primary s_down asks the others (SENTINEL is-master-down-by-addr); with at
 *      least `quorum` monitors (itself included) agreeing, the primary is objectively down (o_down).
 *   3. The monitor then starts an election: it increments the current epoch, votes for itself and asks
 *      the others for their vote in that epoch. A monitor votes at most once per epoch, for the first
 *      requester, and stores the vote before answering, so that a restart cannot vote twice. A requester
 *      with the votes of a majority of all monitors (and at least `quorum`) leads the failover of that
 *      epoch; others wait failover_timeout_ms before a new attempt.
 *   4. The leader promotes the reachable replica with the highest replication offset (REPLICAOF NO ONE),
 *      waits until it reports the primary role, records the new primary with the election epoch as
 *      configuration epoch, tells the other monitors, and points the other replicas at it.
 *   5. Monitors exchange their configuration every second and adopt a higher configuration epoch. A
 *      monitor that reaches a majority of all monitors and holds the newest configuration it has seen
 *      points every listed node that is not the configured primary at it: a former primary that comes
 *      back becomes a replica of the new one.
 *
 * Clients find the primary with SENTINEL GET-MASTER-ADDR-BY-NAME or SENTINEL MASTERS (the Redis Sentinel
 * client protocol, as used by redis-py's Sentinel); SENTINEL REPLICAS and SENTINEL SENTINELS list the
 * other members. SENTINEL FAILOVER starts an election without waiting for o_down (it still needs the
 * votes of a majority).
 *
 * Writes acknowledged by a primary that the promoted replica had not received are lost, as with manual
 * failover (OPERATIONS.md); WAIT bounds that for the writes that need it. A primary cut off from its
 * monitors but reachable by clients keeps accepting writes until it is demoted.
 */
#ifndef AKV_SENTINEL_H
#define AKV_SENTINEL_H

#include <stddef.h>
#include <stdint.h>

#define SENTINEL_MAX_NODES 32
#define SENTINEL_MAX_PEERS 16
#define SENTINEL_ID_LEN 40 /* hexadecimal digits of a monitor identity */

typedef struct sentinel_node_cfg {
    char host[256];  /* address used by the monitors and by replicas (REPLICAOF) */
    int resp_port;   /* RESP port: monitoring and commands */
    int native_port; /* native port: the target of REPLICAOF */
} sentinel_node_cfg;

typedef struct sentinel_peer_cfg {
    char host[256];
    int port;
} sentinel_peer_cfg;

typedef struct sentinel_cfg {
    char name[64]; /* group name used by clients */
    char listen_addr[256];
    char listen_port[16];
    sentinel_node_cfg nodes[SENTINEL_MAX_NODES]; /* nodes[0] is the primary until a failover */
    unsigned nnodes;
    sentinel_peer_cfg peers[SENTINEL_MAX_PEERS]; /* the other monitors */
    unsigned npeers;
    unsigned quorum;              /* monitors that must see the primary down (default: majority) */
    unsigned down_after_ms;       /* default 5000 */
    unsigned failover_timeout_ms; /* default 60000 */
    char state_path[1024];        /* epochs, vote and configuration across restarts */
    char node_user[128];          /* credentials for the nodes (AUTH); empty = none */
    char node_pass[256];
} sentinel_cfg;

/* Sets defaults (quorum 0 = majority of all monitors, down-after 5 s, failover timeout 60 s). */
void sentinel_cfg_default(sentinel_cfg *cfg);

typedef struct sentinel sentinel;

/*
 * Loads the state file (creating identity and state on first use), opens the listening socket and starts
 * the threads. Returns NULL with a message in err.
 */
sentinel *sentinel_start(const sentinel_cfg *cfg, char *err, size_t errlen);

/* Port the monitor listens on (useful with port "0"). */
int sentinel_port(const sentinel *s);

/* Stops the threads and frees the monitor. */
void sentinel_stop(sentinel *s);

/* ---- decision rules (pure functions, unit-tested) ------------------------------------------------- */

/* A replica as seen by the monitor for the choice of the promoted replica. */
typedef struct sentinel_candidate {
    int reachable;   /* answered within down_after_ms */
    int is_replica;  /* reports the replica role */
    uint64_t offset; /* replication offset it reported */
} sentinel_candidate;

/* Index of the replica to promote (highest offset among reachable replicas, the lowest index on a tie;
 * the primary at index `primary` is skipped), or -1 when none qualifies. */
int sentinel_pick(const sentinel_candidate *c, unsigned n, unsigned primary);

/* Votes needed to lead a failover: a majority of all monitors, and at least the quorum. */
unsigned sentinel_votes_needed(unsigned monitors, unsigned quorum);

/* Vote state of a monitor. */
typedef struct sentinel_vote {
    uint64_t current_epoch;
    uint64_t voted_epoch;
    char voted_for[SENTINEL_ID_LEN + 1];
} sentinel_vote;

/*
 * Applies a vote request for epoch `epoch` from monitor `id`: the current epoch rises to the request's,
 * and the vote of an epoch not yet voted in goes to the requester. Returns 1 when the state changed (it
 * must be stored before the answer is sent), 0 otherwise. The answer is voted_for and voted_epoch.
 */
int sentinel_vote_request(sentinel_vote *v, uint64_t epoch, const char *id);

#endif
