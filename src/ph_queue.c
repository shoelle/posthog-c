#include "ph_queue.h"

#include <stdlib.h>
#include <string.h>

#define PH_QUEUE_MIN_CAP 8

int ph_queue_init(ph_queue *q, int cap) {
    memset(q, 0, sizeof(*q));
    if (cap < PH_QUEUE_MIN_CAP) cap = PH_QUEUE_MIN_CAP;
    q->slots = (ph_event *)calloc((size_t)cap, sizeof(ph_event));
    if (!q->slots) return -1;
    q->cap = cap;
    ph_mutex_init(&q->lock);
    ph_cond_init(&q->not_empty);
    return 0;
}

void ph_queue_free(ph_queue *q) {
    if (q->slots) {
        ph_cond_destroy(&q->not_empty);
        ph_mutex_destroy(&q->lock);
        free(q->slots);
        q->slots = NULL;
    }
}

ph_event *ph_queue_begin_push(ph_queue *q) {
    int idx;
    ph_mutex_lock(&q->lock);
    if (q->size == q->cap) {
        /* Drop the oldest to make room. */
        q->head = (q->head + 1) % q->cap;
        q->size--;
        q->dropped++;
    }
    idx = (q->head + q->size) % q->cap; /* the tail slot */
    return &q->slots[idx];
}

void ph_queue_end_push(ph_queue *q) {
    q->size++;
    ph_mutex_unlock(&q->lock);
}

int ph_queue_pop_batch(ph_queue *q, ph_event *out, int max) {
    int n = 0;
    ph_mutex_lock(&q->lock);
    while (n < max && q->size > 0) {
        out[n] = q->slots[q->head];
        q->head = (q->head + 1) % q->cap;
        q->size--;
        n++;
    }
    ph_mutex_unlock(&q->lock);
    return n;
}

void ph_queue_wait(ph_queue *q, int threshold, int timeout_ms) {
    ph_mutex_lock(&q->lock);
    if (q->size < threshold && !q->woken) {
        /* A producer does not wake the sender for every event: batching waits
         * for either the threshold, the interval timeout, or an explicit wake
         * from flush/shutdown/threshold-crossing logic. The `woken` latch closes
         * the race where an explicit wake lands between two waits. */
        ph_cond_timedwait(&q->not_empty, &q->lock, timeout_ms);
    }
    q->woken = 0;
    ph_mutex_unlock(&q->lock);
}

void ph_queue_wake(ph_queue *q) {
    ph_mutex_lock(&q->lock);
    q->woken = 1;
    ph_cond_broadcast(&q->not_empty);
    ph_mutex_unlock(&q->lock);
}

int ph_queue_size(ph_queue *q) {
    int n;
    ph_mutex_lock(&q->lock);
    n = q->size;
    ph_mutex_unlock(&q->lock);
    return n;
}

uint64_t ph_queue_dropped(ph_queue *q) {
    uint64_t d;
    ph_mutex_lock(&q->lock);
    d = q->dropped;
    ph_mutex_unlock(&q->lock);
    return d;
}
