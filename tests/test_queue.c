#include "ph_queue.h"
#include "test_util.h"

#include <string.h>

static void push_seq(ph_queue *q, uint64_t seq) {
    ph_event *e = ph_queue_begin_push(q);
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    ph_queue_end_push(q);
}

void suite_queue(void) {
    ph_queue q;
    ph_event out[16];
    int i, n;

    CHECK(ph_queue_init(&q, 8) == 0);
    CHECK(ph_queue_size(&q) == 0);

    for (i = 0; i < 8; i++) push_seq(&q, (uint64_t)i);
    CHECK(ph_queue_size(&q) == 8);
    CHECK(ph_queue_dropped(&q) == 0);

    /* one past capacity evicts the oldest (seq 0) */
    push_seq(&q, 99);
    CHECK(ph_queue_size(&q) == 8);
    CHECK(ph_queue_dropped(&q) == 1);

    n = ph_queue_pop_batch(&q, out, 16);
    CHECK(n == 8);
    CHECK(out[0].seq == 1);  /* seq 0 was dropped */
    CHECK(out[7].seq == 99); /* newest preserved */
    CHECK(ph_queue_size(&q) == 0);

    n = ph_queue_pop_batch(&q, out, 16);
    CHECK(n == 0);

    /* pop respects the max limit */
    for (i = 0; i < 5; i++) push_seq(&q, (uint64_t)i);
    n = ph_queue_pop_batch(&q, out, 3);
    CHECK(n == 3);
    CHECK(ph_queue_size(&q) == 2);

    ph_queue_free(&q);
}
