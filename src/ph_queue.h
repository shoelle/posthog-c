/*
 * ph_queue.h — the bounded, drop-oldest ring the capture path feeds.
 *
 * The ring is allocated once at init; ph_capture() never allocates. When the
 * ring is full a push evicts the oldest event and bumps a dropped counter, so
 * a producer that outruns the network degrades gracefully instead of blocking
 * the caller or growing without bound (§6).
 *
 * The event record lives here because it is the ring's element type. It is a
 * plain POD with a single fixed data[] blob (name + optional distinct_id
 * override + packed properties), so a slot is trivially copyable.
 */
#ifndef PH_QUEUE_H
#define PH_QUEUE_H

#include "posthog.h"
#include "ph_thread.h"

#include <stdint.h>

#ifndef PH_EVENT_DATA_CAP
#define PH_EVENT_DATA_CAP 2048
#endif

typedef enum ph_event_kind {
    PH_EV_CAPTURE = 0,   /* ordinary event */
    PH_EV_IDENTIFY = 1,  /* $identify (+ $set) */
    PH_EV_ALIAS = 2,     /* $create_alias */
    PH_EV_GROUP = 3,     /* $groupidentify */
    PH_EV_EXCEPTION = 4  /* $exception */
} ph_event_kind;

enum {
    PH_EVF_NO_PROFILE = 1u << 0, /* force $process_person_profile:false */
    PH_EVF_HAS_DID = 1u << 1     /* data[] carries a distinct_id override */
};

/*
 * data[] layout: [name: name_len][distinct_id: did_len][packed props: blob_len]
 *
 * Each packed property entry is:
 *   u8  type   (ph_prop_type)
 *   u8  klen   (key length, no NUL)
 *   u16 vlen   (STR: byte length; DOUBLE/INT: 8; BOOL: 1)
 *   u8  key[klen]
 *   u8  val[vlen]  (raw host-endian bytes for numbers; UTF-8 for strings)
 */
typedef struct ph_event {
    uint8_t kind;      /* ph_event_kind */
    uint8_t flags;     /* PH_EVF_* */
    uint16_t name_len;
    uint16_t did_len;  /* distinct_id override length (0 => use ctx id) */
    uint16_t blob_len; /* packed-props length */
    uint64_t mono_ns;  /* monotonic tick captured on the caller thread */
    uint64_t seq;      /* per-event sequence; feeds uuid + preserves order */
    char data[PH_EVENT_DATA_CAP];
} ph_event;

typedef struct ph_queue {
    ph_event *slots;
    int cap;
    int head; /* index of the oldest event */
    int size; /* number of live events */
    uint64_t dropped; /* lifetime count of drop-oldest evictions */
    int woken;        /* wake() latch, so a wake between waits isn't lost */
    ph_mutex lock;
    ph_cond not_empty;
} ph_queue;

/* Allocate the ring (cap is clamped to a small floor). Returns 0 on success. */
int ph_queue_init(ph_queue *q, int cap);
void ph_queue_free(ph_queue *q);

/*
 * Producer path. begin_push locks the queue, evicts the oldest event if the
 * ring is full, and returns the slot to fill in place (no copy). The caller
 * fills the returned record, then calls end_push to publish it and wake the
 * consumer. The lock is held between the two calls, so keep filling short.
 */
ph_event *ph_queue_begin_push(ph_queue *q);
void ph_queue_end_push(ph_queue *q);

/* Consumer path. Copies up to `max` oldest events into `out`, removes them,
 * and returns the count copied (0 if empty). Thread-safe. */
int ph_queue_pop_batch(ph_queue *q, ph_event *out, int max);

/* Block until the queue holds >= `threshold` events, `timeout_ms` elapses, or
 * a wake() arrives. `timeout_ms` < 0 waits indefinitely. */
void ph_queue_wait(ph_queue *q, int threshold, int timeout_ms);

/* Wake any thread parked in ph_queue_wait (used for flush/shutdown). */
void ph_queue_wake(ph_queue *q);

int ph_queue_size(ph_queue *q);
uint64_t ph_queue_dropped(ph_queue *q);

#endif /* PH_QUEUE_H */
