#include "trace_timewheel.h"
#include <rte_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTE_LOGTYPE_TIMEWHEEL RTE_LOGTYPE_USER1

static void parse_line(network_params_t *entry, char *line) {
    char *p = line;

    uint64_t delay = strtoull(p, &p, 10);
    p++;
    uint64_t bw = strtoull(p, &p, 10);
    p++;
    double loss = strtod(p, &p) / 100.0;
    p++;
    uint16_t qcap = (uint16_t)strtoul(p, &p, 10);
    p++;
    uint16_t route = (uint16_t)strtoul(p, &p, 10);

    // Convert bandwidth (bps) to fixed-point cycles per byte.
    // UINT64_MAX is interpreted as zero bandwidth.
    uint64_t bytes_per_sec = bw / 8;
    if (bytes_per_sec == 0) {
        entry->cycles_per_byte_fp = UINT64_MAX;
    } else {
        entry->cycles_per_byte_fp =
            (rte_get_tsc_hz() << FP_SHIFT) / bytes_per_sec;
    }

    // Map loss probability [0.0, 1.0] to [0, UINT64_MAX] for direct
    // comparison against xorshift output in the fast path.
    if (loss >= 1.0) {
        entry->loss_threshold = UINT64_MAX;
    } else if (loss <= 0.0) {
        entry->loss_threshold = 0;
    } else {
        entry->loss_threshold = (uint64_t)(loss * (double)UINT64_MAX);
    }

    entry->delay_us = delay;
    entry->queue_cap = qcap;
    entry->route_id = route;
}

trace_timewheel_t *trace_timewheel_create(const char *csv_file) {

    FILE *fp = fopen(csv_file, "r");
    if (!fp) {
        RTE_LOG(ERR, TIMEWHEEL, "Failed to open: %s\n", csv_file);
        return NULL;
    }

    char line[256];
    uint32_t n_lines = 0;
    while (fgets(line, sizeof(line), fp)) {
        n_lines++;
    }

    if (n_lines == 0) {
        RTE_LOG(ERR, TIMEWHEEL, "No entries in: %s\n", csv_file);
        fclose(fp);
        return NULL;
    }

    // Round up to power-of-2 so tick index can use bitmask instead of modulo
    uint32_t size_pow2 = rte_align32pow2(n_lines);
    rewind(fp);

    network_params_t *entries = rte_malloc(
        "tw_entries", size_pow2 * sizeof(*entries), RTE_CACHE_LINE_SIZE
    );
    if (!entries) {
        RTE_LOG(ERR, TIMEWHEEL, "Failed to allocate entries\n");
        fclose(fp);
        return NULL;
    }

    uint32_t count = 0;
    while (fgets(line, sizeof(line), fp)) {
        parse_line(&entries[count++], line);
    }
    fclose(fp);

    // Pad remaining slots with the last entry so wraparound is safe
    for (uint32_t i = count; i < size_pow2; i++) {
        entries[i] = entries[count - 1];
    }

    trace_timewheel_t *tw =
        rte_zmalloc("timewheel", sizeof(*tw), RTE_CACHE_LINE_SIZE);
    if (!tw) {
        RTE_LOG(ERR, TIMEWHEEL, "Failed to allocate timewheel\n");
        rte_free(entries);
        return NULL;
    }

    tw->entries = entries;
    tw->size = size_pow2;
    tw->mask = size_pow2 - 1;

    tw->ticks_per_cycle_fp48 =
        (((__uint128_t)1) << FP_SHIFT_48) /
        ((rte_get_tsc_hz() / 1000) * TIMEWHEEL_TICK_INTERVAL_MS);

    return tw;
}

void trace_timewheel_destroy(trace_timewheel_t *tw) {
    if (!tw) {
        return;
    }
    rte_free(tw->entries);
    rte_free(tw);
}
