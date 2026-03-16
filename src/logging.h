#ifndef LOGGING_H
#define LOGGING_H

#include <rte_common.h>
#include <stdint.h>

#define LATENCY_HISTOGRAM_SIZE 10000 // so we can see up to 10ms

typedef struct {
    uint64_t rx_success;
    uint64_t rx_dropped;
    uint64_t tx_success;
    uint64_t tx_dropped;
    uint64_t tx_dropped_unexpected;
    uint64_t latency_histogram[LATENCY_HISTOGRAM_SIZE];
} __rte_cache_aligned worker_stats_t;

void worker_stats_reset(worker_stats_t *stats);
void worker_stats_print(const char *name, const worker_stats_t *stats);
int worker_stats_save(const worker_stats_t *stats, const char *filepath);

#endif /* LOGGING_H */
