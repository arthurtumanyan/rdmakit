/*
 * rdma.c - RDMA transport: reliable-connection send/receive over rdma_cm.
 *
 * Resources per endpoint: one completion queue shared by both work queues, one
 * reliable-connection queue pair, and one anonymous mapping holding
 *   [0, arena_len)                 message arena, registered with local write access
 *   [credit_off, credit_off + 4K)  credit page, registered with local and remote write access
 *
 * Arena layout (slot size = local max_msg):
 *   [0, recv_depth)                        receive slots, all posted at creation
 *   [recv_depth, recv_depth + send_depth)  send ring slots
 *
 * Work request ids: slot index in the low 32 bits; bit 32 set for send-queue requests.
 *
 * Flow control. Each side posts recv_depth receive buffers. A sender may have at most
 * peer_recv_depth messages outstanding, where "outstanding" means sent and not yet released by the
 * receiving application. Credits are cumulative counters, so duplicates and reordering are harmless:
 *   released_total   messages this side has released (receive slot re-posted), wraps modulo 2^32
 *   peer_ack_total   highest released_total value learned from the peer
 *   sent_total       data messages sent
 *   credits          peer_recv_depth - (sent_total - peer_ack_total)
 * The counter reaches the peer through two paths: every data message carries released_total in its
 * immediate data, and an RDMA WRITE of the counter into the peer's credit word is issued when no data
 * message is available to carry it (batched: at least recv_depth / 2 unannounced releases, or
 * immediately when the application is about to wait). The credit write consumes no receive buffer at
 * the peer, so no receive slot has to be reserved and receiver-not-ready conditions cannot occur.
 * The remote write right covers only the credit page of the peer's own connection.
 *
 * Send ring. Send slots are used in order. Only every signal_interval-th work request is signaled;
 * reliable-connection completions are ordered, so a signaled completion releases every earlier slot.
 * Payloads up to max_inline bytes are sent inline and are not copied into the arena.
 *
 * Wait modes. In polling mode a blocked call spins on the completion queue. In event mode a blocked call
 * arms the completion queue, re-checks it to close the race with completions that arrived before arming,
 * and sleeps in ppoll() on the completion channel and on the connection-manager channel. Credit-word
 * updates raise no completion, so a call blocked on credits wakes every CREDIT_RECHECK_NS to read the
 * word.
 *
 * Event-loop descriptor. Every endpoint has a completion channel (in both wait modes) and an epoll
 * descriptor that contains the channel, the connection-manager channel and a timerfd. duct_ep_arm()
 * arms the completion queue and, while a sender is blocked on credits or a disconnect grace period
 * runs, the timer, which replaces the timed re-check of the blocking event mode.
 *
 * Posted operations. Zero-copy sends, RDMA READ/WRITE and atomics occupy one send ring slot each and are
 * always signaled. slot_seq records the sequence number of the operation in a slot; reclaiming a slot
 * advances the completed sequence number. Reliable-connection send queues complete in order, so the
 * number is cumulative. Remote access violations complete with an error status and fail the endpoint
 * with DUCT_EACCESS.
 *
 * Device binding: the context binds to the device of the first connection (protection domain
 * allocated once). Connections on other devices are rejected with DUCT_ENODEV.
 */
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

#define SEND_FLAG (1ull << 32)
#define POLL_BATCH 8
#define MAX_CHUNK 16
#define INLINE_REQUEST 64
#define DISCONNECT_CHECK_INTERVAL 256u
#define CREDIT_PAGE 4096u
#define CREDIT_RECHECK_NS 200000L
#define CLOSE_GRACE_MS 3
/* Upper bound for outstanding RDMA READ and atomic operations per connection (negotiated with the device). */
#define MAX_RD_ATOM 16

struct rdma_dev {
    pthread_mutex_t lock;
    struct ibv_context *verbs;
    struct ibv_pd *pd;
};

struct rdma_ready {
    uint32_t slot;
    uint32_t len;
};

struct rdma_ep {
    struct duct_ep base;
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_cq *cq;
    struct ibv_comp_channel *chan;
    int epfd; /* event-loop descriptor: chan, ec and tfd */
    int tfd;  /* timerfd for credit re-checks and the disconnect grace period */
    int timer_armed;
    uint8_t rd_atom; /* outstanding READ/atomic operations negotiated for this connection */
    struct ibv_mr *arena_mr;
    struct ibv_mr *credit_mr;
    uint8_t *map;
    size_t map_len;
    size_t arena_len;
    size_t credit_off;
    volatile uint32_t *credit_word; /* written by the peer, little-endian cumulative counter */
    uint32_t slot_size;
    uint32_t recv_depth;
    uint32_t send_depth;
    uint32_t max_inline;

    /* send ring */
    uint32_t send_head; /* next slot to use */
    uint32_t send_tail; /* oldest slot not yet reclaimed */
    uint32_t send_inflight;
    uint32_t since_signal;
    uint32_t signal_interval;
    uint64_t
        *slot_seq;   /* sequence number of the posted operation in each send slot, 0 for internal requests */
    uint64_t posted; /* sequence number of the latest posted operation */
    uint64_t done;   /* sequence number of the latest completed operation */
    int credit_blocked; /* a send found no credits and has not succeeded since */

    /* flow control */
    uint32_t peer_recv_depth;
    uint64_t peer_credit_addr;
    uint32_t peer_credit_rkey;
    uint32_t sent_total;
    uint32_t peer_ack_total;
    uint32_t released_total;
    uint32_t announced_total;
    uint32_t credit_thresh;
    uint32_t announce_word; /* source of the inline credit write */

    /* received, undelivered messages */
    struct rdma_ready *ready;
    uint32_t rhead;
    uint32_t rcount;

    unsigned idle_polls;
    int event_mode;
    int peer_closed; /* disconnect event seen; the endpoint fails once the completion queue stays empty */
    int64_t close_deadline; /* end of the grace period that follows the first empty poll after a disconnect */
    int connected;
    int broken; /* sticky error (negative code) */
};

struct rdma_listener {
    struct duct_listener base;
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
};

int duct_rdma_available(void) {
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (list == NULL) {
        return 0;
    }
    ibv_free_device_list(list);
    return n > 0;
}

int duct_rdma_ctx_init(duct_ctx *ctx) {
    if (!duct_rdma_available()) {
        return DUCT_ENODEV;
    }
    struct rdma_dev *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return -ENOMEM;
    }
    pthread_mutex_init(&d->lock, NULL);
    ctx->rdma = d;
    return 0;
}

void duct_rdma_ctx_fini(duct_ctx *ctx) {
    struct rdma_dev *d = ctx->rdma;
    if (d == NULL) {
        return;
    }
    if (d->pd != NULL) {
        ibv_dealloc_pd(d->pd);
    }
    pthread_mutex_destroy(&d->lock);
    free(d);
    ctx->rdma = NULL;
}

/* Binds the context to the device of a connection id and returns its protection domain. */
static int dev_bind(duct_ctx *ctx, struct ibv_context *verbs, struct ibv_pd **pd) {
    struct rdma_dev *d = ctx->rdma;
    int rc = 0;
    pthread_mutex_lock(&d->lock);
    if (d->verbs == NULL) {
        d->pd = ibv_alloc_pd(verbs);
        if (d->pd == NULL) {
            rc = -errno;
        } else {
            d->verbs = verbs;
        }
    } else if (d->verbs != verbs) {
        rc = DUCT_ENODEV;
    }
    *pd = d->pd;
    pthread_mutex_unlock(&d->lock);
    return rc;
}

/* Maps a CM event that differs from the expected one to an error code. */
static int cm_event_error(const struct rdma_cm_event *ev) {
    switch (ev->event) {
    case RDMA_CM_EVENT_REJECTED:
        return -ECONNREFUSED;
    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
        return -EHOSTUNREACH;
    case RDMA_CM_EVENT_CONNECT_ERROR:
        return -ECONNRESET;
    case RDMA_CM_EVENT_DISCONNECTED:
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        return DUCT_ECLOSED;
    default:
        return DUCT_EPROTO;
    }
}

/*
 * Waits for the connection-manager event `want`. On success *evp holds the event, which the caller
 * must acknowledge with rdma_ack_cm_event(). On failure the event (if any) is acknowledged here.
 */
static int cm_wait(struct rdma_event_channel *ec, enum rdma_cm_event_type want, int64_t deadline,
                   struct rdma_cm_event **evp) {
    struct pollfd p = {.fd = ec->fd, .events = POLLIN};
    for (;;) {
        int rc = poll(&p, 1, duct_remaining_ms(deadline));
        if (rc > 0) {
            break;
        }
        if (rc == 0) {
            return -ETIMEDOUT;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev) != 0) {
        return -errno;
    }
    if (ev->event != want) {
        int err = cm_event_error(ev);
        rdma_ack_cm_event(ev);
        return err;
    }
    *evp = ev;
    return 0;
}

static int cm_wait_ack(struct rdma_event_channel *ec, enum rdma_cm_event_type want, int64_t deadline) {
    struct rdma_cm_event *ev;
    int rc = cm_wait(ec, want, deadline, &ev);
    if (rc == 0) {
        rdma_ack_cm_event(ev);
    }
    return rc;
}

/* Posts receive slot `slot`. */
static int post_recv(struct rdma_ep *e, uint32_t slot) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)(e->map + (size_t)slot * e->slot_size),
        .length = e->slot_size,
        .lkey = e->arena_mr->lkey,
    };
    struct ibv_recv_wr wr = {.wr_id = slot, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad;
    int rc = ibv_post_recv(e->id->qp, &wr, &bad);
    return rc == 0 ? 0 : -rc;
}

/* Send ring slots currently unused. */
static inline uint32_t ring_free(const struct rdma_ep *e) {
    return e->send_depth - e->send_inflight;
}

/* Send credits currently available (messages that may be sent without overrunning the peer). */
static inline uint32_t credits_available(const struct rdma_ep *e) {
    return e->peer_recv_depth - (e->sent_total - e->peer_ack_total);
}

/* Returns the buffer of send slot `slot`. */
static inline uint8_t *send_buf(const struct rdma_ep *e, uint32_t slot) {
    return e->map + (size_t)(e->recv_depth + slot) * e->slot_size;
}

/* Decides whether the next send-queue request is signaled. */
static inline int want_signal(struct rdma_ep *e) {
    if (++e->since_signal >= e->signal_interval) {
        e->since_signal = 0;
        return 1;
    }
    return 0;
}

/*
 * Records a release counter announced by the peer. The counter is cumulative and monotonic; values
 * that would acknowledge more messages than were sent violate the protocol.
 */
static void absorb_ack(struct rdma_ep *e, uint32_t v) {
    uint32_t advance = v - e->peer_ack_total;
    if (advance == 0 || advance > UINT32_MAX / 2) {
        return; /* no news, or a stale value that arrived out of order */
    }
    if (advance > e->sent_total - e->peer_ack_total) {
        e->broken = DUCT_EPROTO;
        return;
    }
    e->peer_ack_total = v;
}

/* Marks send ring slots up to and including `slot` as reusable and advances the completed sequence number. */
static void ring_reclaim(struct rdma_ep *e, uint32_t slot) {
    uint32_t n = (slot + e->send_depth - e->send_tail) % e->send_depth + 1;
    for (uint32_t i = 0, s = e->send_tail; i < n; i++, s = (s + 1) % e->send_depth) {
        if (e->slot_seq[s] != 0) {
            e->done = e->slot_seq[s];
            e->slot_seq[s] = 0;
        }
    }
    e->send_tail = (slot + 1) % e->send_depth;
    e->send_inflight -= n;
}

/*
 * Checks the endpoint's event channel without blocking and records a peer disconnect. The endpoint is
 * not failed here: completions of messages that arrived before the disconnect must still be delivered.
 */
static void ep_check_disconnect(struct rdma_ep *e) {
    struct rdma_cm_event *ev;
    while (rdma_get_cm_event(e->ec, &ev) == 0) {
        enum rdma_cm_event_type t = ev->event;
        rdma_ack_cm_event(ev);
        if (t == RDMA_CM_EVENT_DISCONNECTED || t == RDMA_CM_EVENT_TIMEWAIT_EXIT ||
            t == RDMA_CM_EVENT_DEVICE_REMOVAL) {
            e->connected = 0;
            e->peer_closed = 1;
        }
    }
}

/*
 * Processes available completions and the credit word. Returns the number of work completions
 * handled, or a negative error (sticky).
 */
static int ep_progress(struct rdma_ep *e) {
    if (e->broken != 0) {
        return e->broken;
    }
    absorb_ack(e, le32toh(__atomic_load_n(e->credit_word, __ATOMIC_ACQUIRE)));
    struct ibv_wc wc[POLL_BATCH];
    int n = ibv_poll_cq(e->cq, POLL_BATCH, wc);
    if (n < 0) {
        e->broken = -EIO;
        return e->broken;
    }
    if (n == 0) {
        if (++e->idle_polls % DISCONNECT_CHECK_INTERVAL == 0) {
            ep_check_disconnect(e);
        }
        if (e->peer_closed) {
            /*
             * Completions of messages that arrived before the disconnect may become visible slightly after
             * the disconnect event; keep polling for CLOSE_GRACE_MS before failing the endpoint.
             */
            if (e->close_deadline == 0) {
                e->close_deadline = duct_now_ms() + CLOSE_GRACE_MS;
            }
            if (duct_now_ms() >= e->close_deadline) {
                e->broken = DUCT_ECLOSED;
            }
        }
        return e->broken;
    }
    e->idle_polls = 0;
    for (int i = 0; i < n; i++) {
        if (wc[i].status != IBV_WC_SUCCESS) {
            switch (wc[i].status) {
            case IBV_WC_WR_FLUSH_ERR:
                e->broken = DUCT_ECLOSED;
                break;
            case IBV_WC_REM_ACCESS_ERR:
            case IBV_WC_REM_INV_REQ_ERR:
            case IBV_WC_LOC_PROT_ERR:
                e->broken = DUCT_EACCESS;
                break;
            default:
                e->broken = DUCT_EWC;
                break;
            }
            return e->broken;
        }
        uint32_t slot = (uint32_t)(wc[i].wr_id & 0xffffffffu);
        if (wc[i].wr_id & SEND_FLAG) {
            ring_reclaim(e, slot);
            continue;
        }
        if (!(wc[i].wc_flags & IBV_WC_WITH_IMM)) {
            e->broken = DUCT_EPROTO;
            return e->broken;
        }
        absorb_ack(e, ntohl(wc[i].imm_data));
        uint32_t tail = (e->rhead + e->rcount) % e->recv_depth;
        e->ready[tail] = (struct rdma_ready){.slot = slot, .len = wc[i].byte_len};
        e->rcount++;
    }
    return e->broken != 0 ? e->broken : n;
}

/*
 * Announces released receive slots to the peer with an RDMA WRITE into its credit word.
 * Without `force` the write is issued only after credit_thresh unannounced releases. Does nothing when
 * the send ring is full; the announcement is retried by later calls. With `marker` the write is issued
 * even when nothing is pending and is always signaled: reliable-connection requests complete in order,
 * so its completion proves that every earlier send was delivered (used when closing).
 */
static void announce_ex(struct rdma_ep *e, int force, int marker) {
    uint32_t pending = e->released_total - e->announced_total;
    if (e->broken != 0 || (pending == 0 && !marker) || (!force && !marker && pending < e->credit_thresh) ||
        ring_free(e) == 0) {
        return;
    }
    uint32_t slot = e->send_head;
    e->announce_word = htole32(e->released_total);
    struct ibv_sge sge = {
        .addr = (uintptr_t)&e->announce_word, .length = sizeof(e->announce_word), .lkey = 0};
    struct ibv_send_wr wr = {
        .wr_id = SEND_FLAG | slot,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_INLINE | ((want_signal(e) || marker) ? IBV_SEND_SIGNALED : 0),
        .wr.rdma = {.remote_addr = e->peer_credit_addr, .rkey = e->peer_credit_rkey},
    };
    struct ibv_send_wr *bad;
    int rc = ibv_post_send(e->id->qp, &wr, &bad);
    if (rc != 0) {
        e->broken = -rc;
        return;
    }
    e->send_head = (slot + 1) % e->send_depth;
    e->send_inflight++;
    e->announced_total = e->released_total;
}

static void announce(struct rdma_ep *e, int force) {
    announce_ex(e, force, 0);
}

/*
 * Sleeps until the completion channel or the connection-manager channel is readable, the deadline
 * passes, or (when credit_wait is set) CREDIT_RECHECK_NS elapse. Event mode only.
 */
static int event_sleep(struct rdma_ep *e, int64_t deadline, int credit_wait) {
    struct pollfd fds[2] = {
        {.fd = e->chan->fd, .events = POLLIN},
        {.fd = e->ec->fd, .events = POLLIN},
    };
    struct timespec ts;
    struct timespec *tsp = NULL;
    int left = duct_remaining_ms(deadline);
    if (left >= 0) {
        ts.tv_sec = left / 1000;
        ts.tv_nsec = (long)(left % 1000) * 1000000L;
        tsp = &ts;
    }
    if (credit_wait && (tsp == NULL || ts.tv_sec > 0 || ts.tv_nsec > CREDIT_RECHECK_NS)) {
        ts.tv_sec = 0;
        ts.tv_nsec = CREDIT_RECHECK_NS;
        tsp = &ts;
    }
    if (e->peer_closed && (tsp == NULL || ts.tv_sec > 0 || ts.tv_nsec > 1000000L)) {
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000L; /* grace period after a disconnect: re-check the queue every millisecond */
        tsp = &ts;
    }
    int rc = ppoll(fds, 2, tsp, NULL);
    if (rc < 0 && errno != EINTR) {
        return -errno;
    }
    if (rc > 0 && (fds[0].revents & POLLIN)) {
        struct ibv_cq *ev_cq;
        void *ev_ctx;
        if (ibv_get_cq_event(e->chan, &ev_cq, &ev_ctx) == 0) {
            ibv_ack_cq_events(ev_cq, 1);
        }
    }
    if (rc > 0 && (fds[1].revents & POLLIN)) {
        ep_check_disconnect(e);
    }
    return 0;
}

/*
 * One iteration of a blocking wait: progress, flush credits, check the deadline, then spin (polling
 * mode) or sleep (event mode). credit_wait marks waits that depend on the credit word.
 */
static int wait_step(struct rdma_ep *e, int64_t deadline, int credit_wait) {
    int rc = ep_progress(e);
    if (rc < 0) {
        return rc;
    }
    if (rc > 0) {
        return 0;
    }
    announce(e, 1);
    if (deadline >= 0 && duct_remaining_ms(deadline) == 0) {
        return -ETIMEDOUT;
    }
    if (!e->event_mode) {
        return 0;
    }
    /* Arm, then look again: a completion that arrived before arming would not raise an event. */
    if (ibv_req_notify_cq(e->cq, 0) != 0) {
        return -errno;
    }
    rc = ep_progress(e);
    if (rc != 0) {
        return rc < 0 ? rc : 0;
    }
    return event_sleep(e, deadline, credit_wait);
}

/* Posts k messages (k <= MAX_CHUNK, credits and ring slots checked by the caller) with one doorbell. */
static int post_chunk(struct rdma_ep *e, const struct duct_msg *msgs, unsigned k) {
    struct ibv_send_wr wr[MAX_CHUNK];
    struct ibv_sge sge[MAX_CHUNK];
    uint32_t imm = htonl(e->released_total);
    uint32_t slot = e->send_head;
    for (unsigned i = 0; i < k; i++) {
        int inl = msgs[i].len <= e->max_inline;
        if (inl) {
            sge[i].addr = (uintptr_t)msgs[i].data;
            sge[i].lkey = 0;
        } else {
            uint8_t *buf = send_buf(e, slot);
            memcpy(buf, msgs[i].data, msgs[i].len);
            sge[i].addr = (uintptr_t)buf;
            sge[i].lkey = e->arena_mr->lkey;
        }
        sge[i].length = (uint32_t)msgs[i].len;
        wr[i] = (struct ibv_send_wr){
            .wr_id = SEND_FLAG | slot,
            .next = i + 1 < k ? &wr[i + 1] : NULL,
            .sg_list = &sge[i],
            .num_sge = msgs[i].len > 0 ? 1 : 0,
            .opcode = IBV_WR_SEND_WITH_IMM,
            .imm_data = imm,
            .send_flags = (inl ? IBV_SEND_INLINE : 0) | (want_signal(e) ? IBV_SEND_SIGNALED : 0),
        };
        slot = (slot + 1) % e->send_depth;
    }
    struct ibv_send_wr *bad;
    int rc = ibv_post_send(e->id->qp, wr, &bad);
    if (rc != 0) {
        e->broken = -rc;
        return -rc;
    }
    e->send_head = slot;
    e->send_inflight += k;
    e->sent_total += k;
    e->announced_total = e->released_total; /* the immediate data carried the current counter */
    e->credit_blocked = 0;
    return 0;
}

static int rdma_send_batch(duct_ep *base, const struct duct_msg *msgs, unsigned n, int timeout_ms) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    unsigned done = 0;
    while (done < n) {
        if (e->broken != 0) {
            return e->broken;
        }
        unsigned k = n - done;
        if (k > MAX_CHUNK) {
            k = MAX_CHUNK;
        }
        if (k > credits_available(e)) {
            k = credits_available(e);
        }
        if (k > ring_free(e)) {
            k = ring_free(e);
        }
        if (k == 0) {
            if (credits_available(e) == 0) {
                e->credit_blocked = 1;
            }
            int rc = wait_step(e, deadline, credits_available(e) == 0);
            if (rc < 0) {
                return rc;
            }
            continue;
        }
        int rc = post_chunk(e, msgs + done, k);
        if (rc != 0) {
            return rc;
        }
        done += k;
    }
    return 0;
}

static int rdma_send(duct_ep *base, const void *data, size_t len, int timeout_ms) {
    struct duct_msg m = {.data = data, .len = len};
    return rdma_send_batch(base, &m, 1, timeout_ms);
}

static int rdma_recv(duct_ep *base, struct duct_rx *rx, int timeout_ms) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    while (e->rcount == 0) {
        int rc = wait_step(e, deadline, 0);
        if (rc < 0 && e->rcount == 0) {
            return rc; /* messages received before the failure are delivered first */
        }
    }
    struct rdma_ready r = e->ready[e->rhead];
    e->rhead = (e->rhead + 1) % e->recv_depth;
    e->rcount--;
    rx->data = e->map + (size_t)r.slot * e->slot_size;
    rx->len = r.len;
    rx->token = r.slot;
    return 0;
}

static int rdma_release(duct_ep *base, struct duct_rx *rx) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    if (rx->data == NULL) {
        return -EINVAL;
    }
    int rc = post_recv(e, (uint32_t)rx->token);
    rx->data = NULL;
    rx->len = 0;
    if (rc != 0) {
        e->broken = rc;
        return rc;
    }
    e->released_total++;
    announce(e, 0);
    return 0;
}

/*
 * Waits until a send ring slot (and, with need_credit, a flow-control credit) is available.
 * Returns 0, -ETIMEDOUT, or a transport error.
 */
static int wait_slot(struct rdma_ep *e, int64_t deadline, int need_credit) {
    for (;;) {
        if (e->broken != 0) {
            return e->broken;
        }
        int credit_ok = !need_credit || credits_available(e) > 0;
        if (ring_free(e) > 0 && credit_ok) {
            return 0;
        }
        if (!credit_ok) {
            e->credit_blocked = 1;
        }
        int rc = wait_step(e, deadline, !credit_ok);
        if (rc < 0) {
            return rc;
        }
    }
}

/* Posts one signaled user operation into the next ring slot and assigns its sequence number. */
static int post_user(struct rdma_ep *e, struct ibv_send_wr *wr, uint64_t *seq) {
    uint32_t slot = e->send_head;
    wr->wr_id = SEND_FLAG | slot;
    wr->send_flags |= IBV_SEND_SIGNALED;
    struct ibv_send_wr *bad;
    int rc = ibv_post_send(e->id->qp, wr, &bad);
    if (rc != 0) {
        e->broken = -rc;
        return -rc;
    }
    e->send_head = (slot + 1) % e->send_depth;
    e->send_inflight++;
    e->slot_seq[slot] = ++e->posted;
    if (seq != NULL) {
        *seq = e->posted;
    }
    return 0;
}

static int rdma_send_mr(duct_ep *base, const duct_mr *mr, size_t off, size_t len, int timeout_ms,
                        uint64_t *seq) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int rc = wait_slot(e, duct_deadline(timeout_ms), 1);
    if (rc != 0) {
        return rc;
    }
    struct ibv_sge sge = {.addr = (uintptr_t)(mr->addr + off), .length = (uint32_t)len, .lkey = mr->lkey};
    struct ibv_send_wr wr = {
        .sg_list = &sge,
        .num_sge = len > 0 ? 1 : 0,
        .opcode = IBV_WR_SEND_WITH_IMM,
        .imm_data = htonl(e->released_total),
    };
    rc = post_user(e, &wr, seq);
    if (rc == 0) {
        e->sent_total++;
        e->announced_total = e->released_total;
        e->credit_blocked = 0;
    }
    return rc;
}

static int rdma_rma(duct_ep *base, const struct duct_rma *op, int timeout_ms, uint64_t *seq) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int rc = wait_slot(e, duct_deadline(timeout_ms), 0);
    if (rc != 0) {
        return rc;
    }
    struct ibv_sge sge = {.addr = (uintptr_t)op->laddr, .length = (uint32_t)op->len, .lkey = op->lkey};
    struct ibv_send_wr wr = {.sg_list = &sge, .num_sge = 1};
    switch (op->op) {
    case DUCT_RMA_READ:
        wr.opcode = IBV_WR_RDMA_READ;
        wr.wr.rdma.remote_addr = op->raddr;
        wr.wr.rdma.rkey = op->rkey;
        break;
    case DUCT_RMA_WRITE:
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.wr.rdma.remote_addr = op->raddr;
        wr.wr.rdma.rkey = op->rkey;
        break;
    case DUCT_RMA_FADD:
        wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
        wr.wr.atomic.remote_addr = op->raddr;
        wr.wr.atomic.rkey = op->rkey;
        wr.wr.atomic.compare_add = op->a;
        break;
    case DUCT_RMA_CSWAP:
        wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        wr.wr.atomic.remote_addr = op->raddr;
        wr.wr.atomic.rkey = op->rkey;
        wr.wr.atomic.compare_add = op->a;
        wr.wr.atomic.swap = op->b;
        break;
    }
    return post_user(e, &wr, seq);
}

static uint64_t rdma_completed(duct_ep *base) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    ep_progress(e);
    return e->done;
}

static int rdma_wait(duct_ep *base, uint64_t seq, int timeout_ms) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int64_t deadline = duct_deadline(timeout_ms);
    while (e->done < seq) {
        int rc = wait_step(e, deadline, 0);
        if (rc < 0 && e->done < seq) {
            return rc;
        }
    }
    return 0;
}

static int rdma_progress(duct_ep *base) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    int rc = ep_progress(e);
    if (rc < 0) {
        return rc;
    }
    announce(e, 0);
    return e->broken;
}

static unsigned rdma_send_space(duct_ep *base) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    if (ep_progress(e) < 0) {
        return 0;
    }
    uint32_t c = credits_available(e);
    uint32_t r = ring_free(e);
    return c < r ? c : r;
}

/* Posts as many messages as credits and ring slots allow, in chunks of rdma_send_batch(). */
static int rdma_send_some(duct_ep *base, const struct duct_msg *msgs, unsigned n) {
    unsigned done = 0;
    while (done < n) {
        unsigned k = rdma_send_space(base);
        if (k == 0) {
            break;
        }
        if (k > n - done) {
            k = n - done;
        }
        int rc = rdma_send_batch(base, msgs + done, k, 0); /* accepted in full: k <= send space */
        if (rc != 0) {
            return rc;
        }
        done += k;
    }
    return (int)done;
}

static int rdma_fd(const duct_ep *base) {
    return ((const struct rdma_ep *)base)->epfd;
}

/* The completion channel, CM channel and timer need the aggregating set: direct waiting uses it. */
static int rdma_direct_fd(duct_ep *base) {
    return rdma_fd(base);
}

static uint32_t rdma_wait_events(const duct_ep *base) {
    (void)base;
    return EPOLLIN;
}

/* Arms (ns > 0) or disarms (ns == 0) the endpoint's timer. */
static void timer_set(struct rdma_ep *e, long ns) {
    if (ns == 0 && !e->timer_armed) {
        return;
    }
    struct itimerspec its = {.it_value = {.tv_sec = 0, .tv_nsec = ns}};
    timerfd_settime(e->tfd, 0, &its, NULL);
    e->timer_armed = ns != 0;
}

static int rdma_arm(duct_ep *base) {
    struct rdma_ep *e = (struct rdma_ep *)base;
    if (e->broken != 0) {
        return -EAGAIN;
    }
    /* Consume pending notifications: completion events must be acknowledged, timer expirations read. */
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    while (ibv_get_cq_event(e->chan, &ev_cq, &ev_ctx) == 0) {
        ibv_ack_cq_events(ev_cq, 1);
    }
    uint64_t expirations;
    if (read(e->tfd, &expirations, sizeof(expirations)) > 0) {
        e->timer_armed = 0;
    }
    ep_check_disconnect(e);
    int rc = ep_progress(e);
    announce(e, 1);
    if (rc != 0 || e->rcount > 0) {
        return -EAGAIN;
    }
    if (e->credit_blocked && credits_available(e) > 0 && ring_free(e) > 0) {
        return -EAGAIN;
    }
    if (ibv_req_notify_cq(e->cq, 0) != 0) {
        return -errno;
    }
    /* Look again: a completion that arrived before arming would not raise an event. */
    rc = ep_progress(e);
    if (rc != 0 || e->rcount > 0) {
        return -EAGAIN;
    }
    if (e->peer_closed) {
        timer_set(e, 1000000L);
    } else if (e->credit_blocked && credits_available(e) == 0) {
        timer_set(e, CREDIT_RECHECK_NS);
    } else {
        timer_set(e, 0);
    }
    return 0;
}

/*
 * Waits (at most 1 s) until every send posted so far has completed, so that closing does not discard
 * messages still in flight. Errors are ignored: the endpoint is being closed anyway.
 */
static void drain_sends(struct rdma_ep *e) {
    if (!e->connected || e->broken != 0 || e->send_inflight == 0) {
        return;
    }
    int64_t deadline = duct_deadline(1000);
    while (e->send_inflight > 0 && e->broken == 0 && ring_free(e) == 0 && duct_remaining_ms(deadline) > 0) {
        ep_progress(e);
    }
    announce_ex(e, 1, 1);
    while (e->send_inflight > 0 && e->broken == 0 && duct_remaining_ms(deadline) > 0) {
        ep_progress(e);
    }
}

static void rdma_ep_free(struct rdma_ep *e) {
    drain_sends(e);
    if (e->connected && e->id != NULL) {
        rdma_disconnect(e->id);
    }
    if (e->id != NULL && e->id->qp != NULL) {
        rdma_destroy_qp(e->id);
    }
    if (e->cq != NULL) {
        ibv_destroy_cq(e->cq);
    }
    if (e->chan != NULL) {
        ibv_destroy_comp_channel(e->chan);
    }
    if (e->epfd >= 0) {
        close(e->epfd);
    }
    if (e->tfd >= 0) {
        close(e->tfd);
    }
    if (e->arena_mr != NULL) {
        ibv_dereg_mr(e->arena_mr);
    }
    if (e->credit_mr != NULL) {
        ibv_dereg_mr(e->credit_mr);
    }
    if (e->map != NULL) {
        munmap(e->map, e->map_len);
    }
    if (e->id != NULL) {
        rdma_destroy_id(e->id);
    }
    if (e->ec != NULL) {
        rdma_destroy_event_channel(e->ec);
    }
    free(e->ready);
    free(e->slot_seq);
    free(e);
}

static void rdma_close(duct_ep *base) {
    rdma_ep_free((struct rdma_ep *)base);
}

static const struct duct_ep_ops rdma_ep_ops = {
    .send = rdma_send,
    .send_batch = rdma_send_batch,
    .send_some = rdma_send_some,
    .recv = rdma_recv,
    .release = rdma_release,
    .close = rdma_close,
    .fd = rdma_fd,
    .arm = rdma_arm,
    .progress = rdma_progress,
    .send_space = rdma_send_space,
    .direct_fd = rdma_direct_fd,
    .wait_events = rdma_wait_events,
    .send_mr = rdma_send_mr,
    .rma = rdma_rma,
    .completed = rdma_completed,
    .wait = rdma_wait,
};

/* Allocates the queue pair, completion queue and registered memory of endpoint e (e->id is set). */
static int ep_resources(duct_ctx *ctx, struct rdma_ep *e) {
    struct ibv_pd *pd;
    int rc = dev_bind(ctx, e->id->verbs, &pd);
    if (rc != 0) {
        return rc;
    }
    e->slot_size = ctx->opts.max_msg;
    e->recv_depth = ctx->opts.recv_depth;
    e->send_depth = ctx->opts.send_depth;
    e->signal_interval = e->send_depth / 4 > 0 ? e->send_depth / 4 : 1;
    e->credit_thresh = e->recv_depth / 2 > 0 ? e->recv_depth / 2 : 1;
    e->arena_len = (size_t)e->slot_size * (e->recv_depth + e->send_depth);
    e->credit_off = (e->arena_len + CREDIT_PAGE - 1) & ~(size_t)(CREDIT_PAGE - 1);
    e->map_len = e->credit_off + CREDIT_PAGE;
    e->map = mmap(NULL, e->map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (e->map == MAP_FAILED) {
        e->map = NULL;
        return -ENOMEM;
    }
    e->credit_word = (volatile uint32_t *)(e->map + e->credit_off);
    e->arena_mr = ibv_reg_mr(pd, e->map, e->arena_len, IBV_ACCESS_LOCAL_WRITE);
    if (e->arena_mr == NULL) {
        return -errno;
    }
    e->credit_mr =
        ibv_reg_mr(pd, e->map + e->credit_off, CREDIT_PAGE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (e->credit_mr == NULL) {
        return -errno;
    }
    e->event_mode = ctx->opts.wait == DUCT_WAIT_EVENT;
    e->chan = ibv_create_comp_channel(e->id->verbs);
    if (e->chan == NULL) {
        return -errno;
    }
    int fl = fcntl(e->chan->fd, F_GETFL);
    if (fl < 0 || fcntl(e->chan->fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return -errno;
    }
    e->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    e->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (e->tfd < 0 || e->epfd < 0) {
        return -errno;
    }
    int watch[3] = {e->chan->fd, e->ec->fd, e->tfd};
    for (int i = 0; i < 3; i++) {
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = watch[i]};
        if (epoll_ctl(e->epfd, EPOLL_CTL_ADD, watch[i], &ev) != 0) {
            return -errno;
        }
    }
    struct ibv_device_attr da;
    if (ibv_query_device(e->id->verbs, &da) == 0) {
        int n = da.max_qp_rd_atom < da.max_qp_init_rd_atom ? da.max_qp_rd_atom : da.max_qp_init_rd_atom;
        e->rd_atom = (uint8_t)(n < 1 ? 1 : n > MAX_RD_ATOM ? MAX_RD_ATOM : n);
    } else {
        e->rd_atom = 1;
    }
    e->cq = ibv_create_cq(e->id->verbs, (int)(e->recv_depth + e->send_depth), NULL, e->chan, 0);
    if (e->cq == NULL) {
        return -errno;
    }
    struct ibv_qp_init_attr qa = {
        .send_cq = e->cq,
        .recv_cq = e->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = e->send_depth,
                .max_recv_wr = e->recv_depth,
                .max_send_sge = 1,
                .max_recv_sge = 1,
                .max_inline_data = INLINE_REQUEST},
    };
    if (rdma_create_qp(e->id, pd, &qa) != 0) {
        return -errno;
    }
    e->max_inline = qa.cap.max_inline_data; /* granted value; the credit write needs 4 bytes */
    if (e->max_inline < sizeof(e->announce_word)) {
        return -ENOTSUP;
    }
    e->ready = malloc(sizeof(struct rdma_ready) * e->recv_depth);
    e->slot_seq = calloc(e->send_depth, sizeof(*e->slot_seq));
    if (e->ready == NULL || e->slot_seq == NULL) {
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < e->recv_depth; i++) {
        rc = post_recv(e, i);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* Adopts the peer's handshake: limits, credit word location, initial credits. */
static int adopt_hello(struct rdma_ep *e, const struct duct_hello *theirs) {
    if (theirs->credit_addr == 0 || theirs->credit_rkey == 0 || theirs->recv_depth < 2) {
        return DUCT_EPROTO;
    }
    /* Send slots are sized by the local limit, so the effective send limit is the smaller of both. */
    e->base.peer_max_msg =
        theirs->max_msg < e->base.ctx->opts.max_msg ? theirs->max_msg : e->base.ctx->opts.max_msg;
    e->peer_recv_depth = theirs->recv_depth;
    e->peer_credit_addr = theirs->credit_addr;
    e->peer_credit_rkey = theirs->credit_rkey;
    return 0;
}

/* Builds the local handshake for endpoint e. */
static struct duct_hello local_hello(const duct_ctx *ctx, const struct rdma_ep *e) {
    return (struct duct_hello){
        .max_msg = ctx->opts.max_msg,
        .recv_depth = ctx->opts.recv_depth,
        .credit_addr = (uintptr_t)e->credit_word,
        .credit_rkey = e->credit_mr->rkey,
    };
}

static struct rdma_ep *ep_alloc(duct_ctx *ctx) {
    struct rdma_ep *e = calloc(1, sizeof(*e));
    if (e != NULL) {
        e->base.ops = &rdma_ep_ops;
        e->base.ctx = ctx;
        e->epfd = -1;
        e->tfd = -1;
    }
    return e;
}

static int set_event_nonblock(struct rdma_event_channel *ec) {
    int fl = fcntl(ec->fd, F_GETFL);
    return (fl < 0 || fcntl(ec->fd, F_SETFL, fl | O_NONBLOCK) < 0) ? -errno : 0;
}

/*
 * Fills connection parameters. initiator and responder are the outstanding READ/atomic operations this
 * side issues and accepts.
 */
static void conn_param(struct rdma_conn_param *p, const uint8_t *hello, uint8_t initiator,
                       uint8_t responder) {
    memset(p, 0, sizeof(*p));
    p->private_data = hello;
    p->private_data_len = DUCT_HELLO_SIZE;
    p->responder_resources = responder;
    p->initiator_depth = initiator;
    p->retry_count = 7;
    p->rnr_retry_count = 7;
}

int duct_rdma_connect(duct_ctx *ctx, const char *addr, const char *port, duct_ep **out) {
    int64_t deadline = duct_deadline(ctx->opts.connect_timeout_ms);
    struct rdma_ep *e = ep_alloc(ctx);
    if (e == NULL) {
        return -ENOMEM;
    }
    int rc = 0;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *ai = NULL;
    if (getaddrinfo(addr, port, &hints, &ai) != 0) {
        rdma_ep_free(e);
        return -EHOSTUNREACH;
    }
    e->ec = rdma_create_event_channel();
    if (e->ec == NULL) {
        rc = -errno;
        goto fail;
    }
    if (rdma_create_id(e->ec, &e->id, NULL, RDMA_PS_TCP) != 0) {
        rc = -errno;
        goto fail;
    }
    if (rdma_resolve_addr(e->id, NULL, ai->ai_addr,
                          duct_remaining_ms(deadline) < 0 ? 10000 : duct_remaining_ms(deadline)) != 0) {
        rc = -errno;
        goto fail;
    }
    if ((rc = cm_wait_ack(e->ec, RDMA_CM_EVENT_ADDR_RESOLVED, deadline)) != 0) {
        goto fail;
    }
    if (rdma_resolve_route(e->id, duct_remaining_ms(deadline) < 0 ? 10000 : duct_remaining_ms(deadline)) !=
        0) {
        rc = -errno;
        goto fail;
    }
    if ((rc = cm_wait_ack(e->ec, RDMA_CM_EVENT_ROUTE_RESOLVED, deadline)) != 0) {
        goto fail;
    }
    if ((rc = ep_resources(ctx, e)) != 0) {
        goto fail;
    }
    uint8_t hello[DUCT_HELLO_SIZE];
    struct duct_hello mine = local_hello(ctx, e);
    duct_hello_encode(&mine, hello);
    struct rdma_conn_param p;
    conn_param(&p, hello, e->rd_atom, e->rd_atom);
    if (rdma_connect(e->id, &p) != 0) {
        rc = -errno;
        goto fail;
    }
    struct rdma_cm_event *ev;
    if ((rc = cm_wait(e->ec, RDMA_CM_EVENT_ESTABLISHED, deadline, &ev)) != 0) {
        goto fail;
    }
    struct duct_hello theirs;
    rc = duct_hello_decode(ev->param.conn.private_data, ev->param.conn.private_data_len, &theirs);
    rdma_ack_cm_event(ev);
    if (rc != 0 || (rc = adopt_hello(e, &theirs)) != 0) {
        goto fail;
    }
    e->connected = 1;
    if ((rc = set_event_nonblock(e->ec)) != 0) {
        goto fail;
    }
    freeaddrinfo(ai);
    *out = &e->base;
    return 0;
fail:
    freeaddrinfo(ai);
    rdma_ep_free(e);
    return rc;
}

static int rdma_listener_accept(duct_listener *base, int timeout_ms, duct_ep **out) {
    struct rdma_listener *l = (struct rdma_listener *)base;
    duct_ctx *ctx = base->ctx;
    int64_t deadline = duct_deadline(timeout_ms);
    struct rdma_cm_event *ev = NULL;
    int rc = cm_wait(l->ec, RDMA_CM_EVENT_CONNECT_REQUEST, deadline, &ev);
    if (rc != 0 || ev == NULL) {
        return rc != 0 ? rc : DUCT_EPROTO; /* cm_wait sets ev whenever it returns 0 */
    }
    struct rdma_cm_id *id = ev->id;
    struct duct_hello theirs;
    rc = duct_hello_decode(ev->param.conn.private_data, ev->param.conn.private_data_len, &theirs);
    /* The peer's initiator depth bounds what this side must accept, and its responder resources what it may issue. */
    uint8_t peer_initiator = ev->param.conn.initiator_depth;
    uint8_t peer_responder = ev->param.conn.responder_resources;
    rdma_ack_cm_event(ev);
    struct rdma_ep *e = ep_alloc(ctx);
    if (e == NULL) {
        rdma_reject(id, NULL, 0);
        rdma_destroy_id(id);
        return -ENOMEM;
    }
    if (rc != 0) {
        rdma_reject(id, NULL, 0);
        rdma_destroy_id(id);
        rdma_ep_free(e);
        return rc;
    }
    e->id = id;
    if ((rc = adopt_hello(e, &theirs)) != 0) {
        rdma_reject(id, NULL, 0);
        goto fail;
    }
    e->ec = rdma_create_event_channel();
    if (e->ec == NULL) {
        rc = -errno;
        rdma_reject(id, NULL, 0);
        goto fail;
    }
    /* Moves the connection to a private event channel so that its events are not seen by the listener. */
    if (rdma_migrate_id(id, e->ec) != 0) {
        rc = -errno;
        rdma_reject(id, NULL, 0);
        goto fail;
    }
    if ((rc = ep_resources(ctx, e)) != 0) {
        rdma_reject(id, NULL, 0);
        goto fail;
    }
    uint8_t hello[DUCT_HELLO_SIZE];
    struct duct_hello mine = local_hello(ctx, e);
    duct_hello_encode(&mine, hello);
    uint8_t initiator = e->rd_atom < peer_responder ? e->rd_atom : peer_responder;
    uint8_t responder = e->rd_atom < peer_initiator ? e->rd_atom : peer_initiator;
    struct rdma_conn_param p;
    conn_param(&p, hello, initiator, responder);
    if (rdma_accept(id, &p) != 0) {
        rc = -errno;
        goto fail;
    }
    if ((rc = cm_wait_ack(e->ec, RDMA_CM_EVENT_ESTABLISHED, duct_deadline(ctx->opts.connect_timeout_ms))) !=
        0) {
        goto fail;
    }
    e->connected = 1;
    if ((rc = set_event_nonblock(e->ec)) != 0) {
        goto fail;
    }
    *out = &e->base;
    return 0;
fail:
    rdma_ep_free(e);
    return rc;
}

static int rdma_listener_port(const duct_listener *base) {
    const struct rdma_listener *l = (const struct rdma_listener *)base;
    return ntohs(rdma_get_src_port(l->id));
}

static void rdma_listener_close(duct_listener *base) {
    struct rdma_listener *l = (struct rdma_listener *)base;
    rdma_destroy_id(l->id);
    rdma_destroy_event_channel(l->ec);
    free(l);
}

static int rdma_listener_fd(const duct_listener *base) {
    return ((const struct rdma_listener *)base)->ec->fd;
}

static const struct duct_listener_ops rdma_listener_ops = {
    .accept = rdma_listener_accept,
    .port = rdma_listener_port,
    .close = rdma_listener_close,
    .fd = rdma_listener_fd,
};

int duct_rdma_listen(duct_ctx *ctx, const char *addr, const char *port, duct_listener **out) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
    struct addrinfo *ai = NULL;
    if (getaddrinfo(addr, port, &hints, &ai) != 0) {
        return -EINVAL;
    }
    struct rdma_listener *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        freeaddrinfo(ai);
        return -ENOMEM;
    }
    l->base.ops = &rdma_listener_ops;
    l->base.ctx = ctx;
    int rc = 0;
    l->ec = rdma_create_event_channel();
    if (l->ec == NULL || rdma_create_id(l->ec, &l->id, NULL, RDMA_PS_TCP) != 0 ||
        rdma_bind_addr(l->id, ai->ai_addr) != 0 || rdma_listen(l->id, 128) != 0) {
        rc = -errno;
        if (l->id != NULL) {
            rdma_destroy_id(l->id);
        }
        if (l->ec != NULL) {
            rdma_destroy_event_channel(l->ec);
        }
        free(l);
    }
    freeaddrinfo(ai);
    if (rc != 0) {
        return rc;
    }
    *out = &l->base;
    return 0;
}

int duct_rdma_mr_reg(duct_ctx *ctx, struct duct_mr *mr) {
    struct rdma_dev *d = ctx->rdma;
    struct ibv_pd *pd = NULL;
    pthread_mutex_lock(&d->lock);
    struct ibv_context *verbs = d->verbs;
    pthread_mutex_unlock(&d->lock);
    if (verbs == NULL) {
        /* Bind to the first device known to librdmacm: its contexts are the ones rdma_cm ids use. */
        int n = 0;
        struct ibv_context **list = rdma_get_devices(&n);
        if (list == NULL || n == 0) {
            if (list != NULL) {
                rdma_free_devices(list);
            }
            return DUCT_ENODEV;
        }
        verbs = list[0];
        rdma_free_devices(list);
    }
    int rc = dev_bind(ctx, verbs, &pd);
    if (rc != 0) {
        return rc;
    }
    int flags = IBV_ACCESS_LOCAL_WRITE;
    if (mr->access & DUCT_ACCESS_REMOTE_READ) {
        flags |= IBV_ACCESS_REMOTE_READ;
    }
    if (mr->access & DUCT_ACCESS_REMOTE_WRITE) {
        flags |= IBV_ACCESS_REMOTE_WRITE;
    }
    if (mr->access & DUCT_ACCESS_REMOTE_ATOMIC) {
        flags |= IBV_ACCESS_REMOTE_ATOMIC;
    }
    struct ibv_mr *ib = ibv_reg_mr(pd, mr->addr, mr->len, flags);
    if (ib == NULL) {
        return -errno;
    }
    mr->ibmr = ib;
    mr->key = ib->rkey;
    mr->lkey = ib->lkey;
    return 0;
}

void duct_rdma_mr_dereg(struct duct_mr *mr) {
    if (mr->ibmr != NULL) {
        ibv_dereg_mr(mr->ibmr);
    }
}
