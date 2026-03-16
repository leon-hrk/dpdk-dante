#include "logging.h"

#include <stdio.h>
#include <string.h>

void worker_stats_reset(worker_stats_t *stats) {
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(worker_stats_t));
}

void worker_stats_print(const char *name, const worker_stats_t *stats) {
    if (stats == NULL) {
        return;
    }

    printf(
        "[%s] RX Success: %llu  TX Success: %llu\n",
        name ? name : "unknown",
        (unsigned long long)stats->rx_success,
        (unsigned long long)stats->tx_success
    );
    fflush(stdout);
}

int worker_stats_save(const worker_stats_t *stats, const char *filepath) {
    if (stats == NULL || filepath == NULL) {
        return -1;
    }

    FILE *fp = fopen(filepath, "w");
    if (fp == NULL) {
        fprintf(stderr, "ERROR: cannot open %s for writing\n", filepath);
        return -1;
    }

    fprintf(fp, "rx_success: %llu\n", (unsigned long long)stats->rx_success);
    fprintf(fp, "rx_dropped: %llu\n", (unsigned long long)stats->rx_dropped);
    fprintf(fp, "tx_success: %llu\n", (unsigned long long)stats->tx_success);
    fprintf(fp, "tx_dropped: %llu\n", (unsigned long long)stats->tx_dropped);
    fprintf(
        fp,
        "tx_dropped_unexpected: %llu\n",
        (unsigned long long)stats->tx_dropped_unexpected
    );
    fprintf(fp, "---\n");

    for (uint32_t i = 0; i < LATENCY_HISTOGRAM_SIZE; i++) {
        if (stats->latency_histogram[i] > 0) {
            fprintf(
                fp,
                "%uus: %llu\n",
                i,
                (unsigned long long)stats->latency_histogram[i]
            );
        }
    }

    fclose(fp);
    return 0;
}
