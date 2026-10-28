/*
 * cluster.h - Redis Cluster protocol over a fixed slot map (internal interface of the server).
 *
 * Keys map to 16384 slots by CRC16 of the key, or of the part between the first '{' and the next '}' when
 * that part is not empty (hash tags), as specified by the Redis Cluster specification ("Keys distribution
 * model"). The slot map comes from a configuration file that every node reads; there is no gossip and no
 * failover of slot owners. The map changes when the file is edited and reloaded (CLUSTER RELOAD) on every
 * node, or slot by slot during resharding (CLUSTER SETSLOT, see proto_resp.c), after which a node writes
 * its map back to its file (cluster_save). File format, one node per line ('#' starts a comment):
 *
 *   node <id> <host> <resp-port> <native-port> primary <slot or start-end> ...
 *   node <id> <host> <resp-port> <native-port> replica-of <primary id>
 *
 * Ids are 1 to 40 characters [A-Za-z0-9_-]. Slot ranges of primaries must not overlap; a primary may own no
 * slot. A node is told its own id with --cluster-node-id. Clients reach nodes through the RESP ports;
 * replicas follow their primary through its native port.
 *
 * The per-slot owner and migration states change while workers read them; they are read and written with
 * relaxed atomic accesses (the accessors below), so a reader sees either the old or the new value of a slot.
 */
#ifndef AKV_CLUSTER_H
#define AKV_CLUSTER_H

#include <stddef.h>
#include <stdint.h>

#define CLUSTER_SLOTS 16384u

/* CRC16 of the Redis Cluster specification (XMODEM: polynomial 0x1021, initial value 0). */
uint16_t cluster_crc16(const void *data, size_t n);

/* Slot of a key (hash tags applied). */
unsigned cluster_keyslot(const void *key, size_t n);

struct cluster_node {
    char id[41];
    char host[256];
    int resp_port;
    int native_port;
    int primary;    /* 1 for a primary, 0 for a replica */
    int primary_of; /* index of the primary of a replica, -1 for a primary */
};

typedef struct cluster {
    struct cluster_node *nodes;
    unsigned n;
    int self;                         /* index of this node, -1 when its id is not in the file */
    int16_t owner[CLUSTER_SLOTS];     /* index of the primary that serves each slot, -1 unassigned */
    int16_t migrating[CLUSTER_SLOTS]; /* this node moves the slot to that node (SETSLOT MIGRATING), or -1 */
    int16_t importing[CLUSTER_SLOTS]; /* this node takes the slot from that node (SETSLOT IMPORTING), or -1 */
} cluster;

static inline int cluster_owner(const cluster *c, unsigned slot) {
    return __atomic_load_n(&c->owner[slot], __ATOMIC_RELAXED);
}

static inline int cluster_migrating(const cluster *c, unsigned slot) {
    return __atomic_load_n(&c->migrating[slot], __ATOMIC_RELAXED);
}

static inline int cluster_importing(const cluster *c, unsigned slot) {
    return __atomic_load_n(&c->importing[slot], __ATOMIC_RELAXED);
}

static inline void cluster_set_owner(cluster *c, unsigned slot, int node) {
    __atomic_store_n(&c->owner[slot], (int16_t)node, __ATOMIC_RELAXED);
}

static inline void cluster_set_migrating(cluster *c, unsigned slot, int node) {
    __atomic_store_n(&c->migrating[slot], (int16_t)node, __ATOMIC_RELAXED);
}

static inline void cluster_set_importing(cluster *c, unsigned slot, int node) {
    __atomic_store_n(&c->importing[slot], (int16_t)node, __ATOMIC_RELAXED);
}

/* Index of the node with id (n bytes), or -1. */
int cluster_find(const cluster *c, const char *id, size_t n);

/* Writes the map (nodes and slot owners) to path in the file format above, replacing the file atomically
 * (write, rename; with sync also fsync before the rename). Returns 0, or -1 with a message in err. */
int cluster_save(const cluster *c, const char *path, int sync, char *err, size_t errlen);

/* Reads a configuration file. Returns the map, or NULL with a message (file and line) in err. */
cluster *cluster_load(const char *path, const char *self_id, char *err, size_t errlen);

void cluster_free(cluster *c);

/* Index of the primary whose slots this node serves: itself, or its primary when it is a replica. */
int cluster_shard_of_self(const cluster *c);

/* Number of slots with an owner. */
unsigned cluster_assigned(const cluster *c);

#endif
