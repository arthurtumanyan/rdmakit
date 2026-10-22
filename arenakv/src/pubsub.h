/*
 * pubsub.h - publish/subscribe registry of the server (internal interface).
 *
 * Subscribers are connections, named by their connection id and the worker that serves them. The registry
 * stores no connection pointers: messages are handed to the delivery function with the worker and the id,
 * and the worker appends them to the connection if it still exists. Thread-safe.
 */
#ifndef AKV_PUBSUB_H
#define AKV_PUBSUB_H

#include <stddef.h>
#include <stdint.h>

typedef struct pubsub pubsub;

/*
 * Hands a formatted frame (n bytes, valid during the call) to the inbox of `worker` for connection
 * conn_id. Called with the registry's read lock held, from the publishing thread. Returns 0 when queued.
 */
typedef int (*pubsub_deliver_fn)(void *arg, unsigned worker, uint64_t conn_id, const char *frame, size_t n);

pubsub *pubsub_create(pubsub_deliver_fn deliver, void *arg);
void pubsub_destroy(pubsub *p);

/* Adds a subscription. Returns 1 when added, 0 when it existed, or -ENOMEM. resp3 selects the frame type. */
int pubsub_subscribe(pubsub *p, const char *ch, size_t n, uint64_t conn_id, unsigned worker, int resp3);
int pubsub_psubscribe(pubsub *p, const char *pat, size_t n, uint64_t conn_id, unsigned worker, int resp3);

/* Removes a subscription. Returns 1 when it existed. */
int pubsub_unsubscribe(pubsub *p, const char *ch, size_t n, uint64_t conn_id);
int pubsub_punsubscribe(pubsub *p, const char *pat, size_t n, uint64_t conn_id);

/* Delivers msg to every subscriber of ch and of every pattern matching ch. Returns the number of receivers. */
size_t pubsub_publish(pubsub *p, const char *ch, size_t cn, const char *msg, size_t mn);

/* Counts the channels with subscribers (matching pat when it is not NULL), calling fn for each. */
size_t pubsub_channels(pubsub *p, const char *pat, size_t pn, int (*fn)(void *arg, const char *ch, size_t n),
                       void *arg);

/* Subscribers of one channel. */
size_t pubsub_numsub(pubsub *p, const char *ch, size_t n);

/* Distinct patterns with subscribers. */
size_t pubsub_numpat(pubsub *p);

#endif
