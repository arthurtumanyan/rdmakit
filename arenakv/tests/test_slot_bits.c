/*
 * test_slot_bits.c - index moves of shards whose tables outgrow the hash bits kept in slot values.
 *
 * Shards larger than 512 MiB keep too few hash bits in a slot value to place it in their largest tables;
 * a move then reads the entry's hash from the arena. Such shards cannot be allocated in a test, so the
 * engine is compiled into this test with the kept hash bits limited to 4: every table beyond 16 buckets
 * takes the reading path. Checked against a reference model with ample memory (exact results):
 * - every key is found right after each doubling, while its slot may be in either table
 * - a random mix of stores, overwrites of other sizes, deletions and lookups over a growing key set
 */
#define AKV_SLOT_HASH_BITS_MAX 4u
#include "../src/akv.c"

#include <stdio.h>

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

static size_t key_text(unsigned i, char *key) {
    return (size_t)snprintf(key, 32, "slot-bits:%u", i);
}

/* Value of version v of key i: v + 1 bytes (1 to 40), so overwrites change the entry size. */
static size_t value_of(unsigned i, unsigned v, unsigned char *val) {
    size_t n = (size_t)(v % 40) + 1;
    for (size_t k = 0; k < n; k++) {
        val[k] = (unsigned char)(i * 31 + v * 7 + k);
    }
    return n;
}

int main(void) {
    akv_config cfg;
    akv_config_default(&cfg);
    cfg.capacity = 64 << 20;
    cfg.shards = 1;
    cfg.hash_seed = 13;
    akv *s = akv_create(&cfg);
    EXPECT(s->shards[0].hash_bits == 4, "kept hash bits %u", s->shards[0].hash_bits);

    enum { KEYS = 40000, OPS = 400000 };
    static unsigned version[KEYS]; /* 0 = absent, else version + 1 */
    char key[32];
    unsigned char val[64];
    unsigned char out[64];
    size_t n;
    uint64_t grows = 0;
    unsigned checked = 0;
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    for (unsigned op = 0; op < OPS; op++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        unsigned limit = op < KEYS ? op + 1 : KEYS; /* the key set grows during the first KEYS operations */
        unsigned i = (unsigned)(rng % limit);
        size_t klen = key_text(i, key);
        unsigned kind = (unsigned)(rng >> 40) % 10;
        if (kind < 6) {
            unsigned v = version[i];
            size_t vlen = value_of(i, v, val);
            EXPECT(akv_put(s, key, klen, val, vlen) == 0, "put %u", i);
            version[i] = v + 1;
        } else if (kind < 7) {
            int rc = akv_del(s, key, klen);
            EXPECT(rc == (version[i] != 0 ? 0 : -ENOENT), "del %u: %d", i, rc);
            version[i] = 0;
        } else {
            int rc = akv_get(s, key, klen, out, sizeof(out), &n);
            if (version[i] == 0) {
                EXPECT(rc == -ENOENT, "get %u: absent key found", i);
            } else {
                size_t vlen = value_of(i, version[i] - 1, val);
                EXPECT(rc == 0 && n == vlen && memcmp(out, val, n) == 0, "get %u: wrong value (rc %d)", i,
                       rc);
            }
        }
        if (s->shards[0].st.index_grows != grows) {
            grows = s->shards[0].st.index_grows;
            unsigned missing = 0;
            for (unsigned j = 0; j < limit; j++) {
                klen = key_text(j, key);
                missing += version[j] != 0 && !akv_exists(s, key, klen);
            }
            EXPECT(missing == 0, "%u keys missing during the move after doubling %llu", missing,
                   (unsigned long long)grows);
            EXPECT(s->shards[0].old != NULL, "no move in progress right after doubling %llu",
                   (unsigned long long)grows);
            checked++;
        }
    }
    akv_stats st;
    akv_stats_get(s, &st);
    unsigned present = 0;
    for (unsigned j = 0; j < KEYS; j++) {
        present += version[j] != 0;
    }
    EXPECT(st.entries == present, "entries %llu, model %u", (unsigned long long)st.entries, present);
    EXPECT(st.index_evictions == 0 && st.evictions == 0, "evictions %llu/%llu with ample memory",
           (unsigned long long)st.evictions, (unsigned long long)st.index_evictions);
    EXPECT(s->shards[0].nbuckets > 16 && checked > 3, "only %u doublings beyond the kept hash bits", checked);
    printf("slot bits: %u doublings checked mid-move, %llu buckets with 4 kept hash bits\n", checked,
           (unsigned long long)s->shards[0].nbuckets);
    akv_destroy(s);
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_slot_bits: ok");
    return 0;
}
