#include "delay_timewheel.h"
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <string.h>

#define RTE_LOGTYPE_DELAY_TW RTE_LOGTYPE_USER2

delay_timewheel_t *delay_timewheel_create() {

    size_t total_entries = (size_t)DELAY_TW_SLOTS * DELAY_TW_BURST_SIZE;

    delay_timewheel_t *tw =
        rte_malloc("delay_tw", sizeof(delay_timewheel_t), RTE_CACHE_LINE_SIZE);
    if (!tw) {
        RTE_LOG(ERR, DELAY_TW, "Failed to allocate timewheel instance\n");
        return NULL;
    }

    memset(tw, 0, sizeof(delay_timewheel_t));

    tw->slots = rte_calloc(
        "delay_tw_slots",
        total_entries,
        sizeof(struct rte_mbuf *),
        RTE_CACHE_LINE_SIZE
    );
    if (!tw->slots) {
        RTE_LOG(
            ERR, DELAY_TW, "Failed to allocate %u delay slots\n", DELAY_TW_SLOTS
        );
        rte_free(tw);
        return NULL;
    }

    tw->slot_timestamps = rte_calloc(
        "delay_tw_ts", total_entries, sizeof(uint64_t), RTE_CACHE_LINE_SIZE
    );
    if (!tw->slot_timestamps) {
        RTE_LOG(ERR, DELAY_TW, "Failed to allocate slot timestamps\n");
        rte_free(tw->slots);
        rte_free(tw);
        return NULL;
    }

    tw->slot_counts = rte_calloc(
        "delay_tw_cnt", DELAY_TW_SLOTS, sizeof(uint8_t), RTE_CACHE_LINE_SIZE
    );
    if (!tw->slot_counts) {
        RTE_LOG(ERR, DELAY_TW, "Failed to allocate slot counts\n");
        rte_free(tw->slot_timestamps);
        rte_free(tw->slots);
        rte_free(tw);
        return NULL;
    }

    tw->route_last_cycles = rte_malloc(
        "delay_tw_routes",
        DELAY_TW_MAX_ROUTES * sizeof(uint64_t),
        RTE_CACHE_LINE_SIZE
    );
    if (!tw->route_last_cycles) {
        RTE_LOG(ERR, DELAY_TW, "Failed to allocate route_last_cycles\n");
        rte_free(tw->slot_counts);
        rte_free(tw->slot_timestamps);
        rte_free(tw->slots);
        rte_free(tw);
        return NULL;
    }

    // Initialize all route timestamps to UINT64_MAX (no previous packet seen)
    memset(tw->route_last_cycles, 0xFF, DELAY_TW_MAX_ROUTES * sizeof(uint64_t));

    uint64_t cycles_per_us = rte_get_tsc_hz() / 1000000;

    tw->slot_mask = DELAY_TW_SLOTS - 1;
    tw->current_slot =
        (uint32_t)(rte_rdtsc() / cycles_per_us) & (DELAY_TW_SLOTS - 1);
    tw->us_per_cycle_fp32 = ((uint64_t)1 << 32) / cycles_per_us;
    tw->initialized = true;

    return tw;
}

void delay_timewheel_destroy(delay_timewheel_t *tw) {
    if (!tw) {
        return;
    }

    if (tw->slots) {
        for (uint32_t i = 0; i < DELAY_TW_SLOTS; i++) {
            uint8_t cnt = tw->slot_counts[i];
            for (uint8_t j = 0; j < cnt; j++) {
                struct rte_mbuf *m = SLOT_PKT(tw, i, j);
                if (m) {
                    rte_pktmbuf_free(m);
                }
            }
        }
        rte_free(tw->slots);
    }

    if (tw->slot_timestamps) {
        rte_free(tw->slot_timestamps);
    }

    if (tw->slot_counts) {
        rte_free(tw->slot_counts);
    }

    if (tw->route_last_cycles) {
        rte_free(tw->route_last_cycles);
    }

    rte_free(tw);
}
