#include "vqueue_ring.h"

#include <rte_malloc.h>

vqueue_ring_t *vqueue_ring_create(int socket_id) {

    vqueue_ring_t *vqr = (vqueue_ring_t *)rte_zmalloc_socket(
        NULL, sizeof(*vqr), RTE_CACHE_LINE_SIZE, socket_id
    );
    if (!vqr) {
        return NULL;
    }

    vqr->pkts = (struct rte_mbuf **)rte_zmalloc_socket(
        NULL,
        MAX_QUEUE_CAP * sizeof(struct rte_mbuf *),
        RTE_CACHE_LINE_SIZE,
        socket_id
    );
    if (!vqr->pkts) {
        rte_free(vqr);
        return NULL;
    }

    vqr->timestamps = (uint64_t *)rte_zmalloc_socket(
        NULL, MAX_QUEUE_CAP * sizeof(uint64_t), RTE_CACHE_LINE_SIZE, socket_id
    );
    if (!vqr->timestamps) {
        rte_free(vqr->pkts);
        rte_free(vqr);
        return NULL;
    }

    vqr->mask = MAX_QUEUE_CAP - 1;
    vqr->capacity = MAX_QUEUE_CAP;
    vqr->head = 0;
    vqr->tail = 0;
    vqr->count = 0;
    return vqr;
}

void vqueue_ring_destroy(vqueue_ring_t *r) {
    if (r) {
        rte_free(r->timestamps);
        rte_free(r->pkts);
        rte_free(r);
    }
}
