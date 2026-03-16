#ifndef VQUEUE_RING_H
#define VQUEUE_RING_H

#include <rte_common.h>
#include <rte_mbuf.h>
#include <stdint.h>

#define MAX_QUEUE_CAP 4096

typedef struct {
    struct rte_mbuf **pkts;
    uint64_t *timestamps;
    uint32_t head;
    uint32_t tail;
    uint32_t mask;
    uint32_t count;
    uint32_t capacity;
} __rte_cache_aligned vqueue_ring_t;

vqueue_ring_t *vqueue_ring_create(int socket_id);
void vqueue_ring_destroy(vqueue_ring_t *r);

static inline uint16_t vqueue_ring_enqueue_burst(
    vqueue_ring_t *r, struct rte_mbuf **mbufs, uint16_t n, uint64_t timestamp
) {
    uint32_t free_slots = r->capacity - r->count;
    if (unlikely(n > free_slots)) {
        n = (uint16_t)free_slots;
    }

    uint32_t head = r->head;
    const uint32_t mask = r->mask;

    for (uint16_t i = 0; i < n; i++) {
        r->pkts[head] = mbufs[i];
        r->timestamps[head] = timestamp;
        head = (head + 1) & mask;
    }

    r->head = head;
    r->count += n;
    return n;
}

static inline int
vqueue_ring_dequeue(vqueue_ring_t *r, struct rte_mbuf **mbuf, uint64_t *ts) {
    if (unlikely(r->count == 0)) {
        return -1;
    }

    *mbuf = r->pkts[r->tail];
    *ts = r->timestamps[r->tail];
    r->tail = (r->tail + 1) & r->mask;
    r->count--;
    return 0;
}

#endif /* VQUEUE_RING_H */
