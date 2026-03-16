#ifndef DELAY_TIMEWHEEL_H
#define DELAY_TIMEWHEEL_H

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_prefetch.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DELAY_TW_BURST_SIZE 64
#define DELAY_TW_SLOTS (1024 * 1024)
#define DELAY_TW_MAX_ROUTES 512
#define DELAY_TW_ROUTE_CYCLES_NONE UINT64_MAX

#define SLOT_PKT(tw, slot, idx)                                                \
    ((tw)->slots[(slot) * DELAY_TW_BURST_SIZE + (idx)])

#define SLOT_TIMESTAMP(tw, slot, idx)                                          \
    ((tw)->slot_timestamps[(slot) * DELAY_TW_BURST_SIZE + (idx)])

typedef struct {
    uint32_t current_slot;
    uint32_t slot_mask;
    uint64_t us_per_cycle_fp32;

    struct rte_mbuf **slots;
    uint64_t *slot_timestamps;
    uint8_t *slot_counts;
    uint64_t *route_last_cycles;
    bool initialized;
} __rte_cache_aligned delay_timewheel_t;

delay_timewheel_t *delay_timewheel_create();
void delay_timewheel_destroy(delay_timewheel_t *tw);

static inline uint64_t
cycles_to_us(uint64_t cycles, uint64_t us_per_cycle_fp32) {
    return (uint64_t)(((__uint128_t)cycles * us_per_cycle_fp32) >> 32);
}

static inline uint32_t cycles_to_slot(delay_timewheel_t *tw, uint64_t cycles) {
    return (uint32_t)cycles_to_us(cycles, tw->us_per_cycle_fp32) &
           tw->slot_mask;
}

static inline void
delay_timewheel_set_start_cycles(delay_timewheel_t *tw, uint64_t start_cycles) {
    if (likely(tw && tw->initialized)) {
        tw->current_slot = cycles_to_slot(tw, start_cycles);
    }
}

static inline int delay_timewheel_insert_at(
    delay_timewheel_t *tw,
    struct rte_mbuf *mbuf,
    uint64_t target_cycles,
    uint64_t timestamp,
    int route_id
) {
    if (unlikely(!tw || !tw->initialized || !mbuf)) {
        return -1;
    }

    uint64_t effective_cycles = target_cycles;

    // Enforce per-route packet ordering: if this packet would be delivered
    // before the previous packet on the same route, delay it to match.
    if (route_id > 0 && likely((uint32_t)route_id < DELAY_TW_MAX_ROUTES)) {
        uint64_t stored = tw->route_last_cycles[route_id];

        if (stored == DELAY_TW_ROUTE_CYCLES_NONE) {
            tw->route_last_cycles[route_id] = target_cycles;

        } else if (target_cycles < stored) {
            effective_cycles = stored;

        } else {
            tw->route_last_cycles[route_id] = target_cycles;
        }
    }

    uint32_t target_slot = cycles_to_slot(tw, effective_cycles);

    // If target_slot is behind current_slot (wrapped past or in the past),
    // clamp to the next slot to avoid inserting into already-drained slots.
    uint32_t dist = (target_slot - tw->current_slot) & tw->slot_mask;
    if (dist > DELAY_TW_SLOTS - 10000) {
        target_slot = (tw->current_slot + 1) & tw->slot_mask;
    }

    uint8_t cnt = tw->slot_counts[target_slot];
    if (unlikely(cnt >= DELAY_TW_BURST_SIZE)) {
        return -1;
    }

    SLOT_PKT(tw, target_slot, cnt) = mbuf;
    SLOT_TIMESTAMP(tw, target_slot, cnt) = timestamp;
    tw->slot_counts[target_slot] = cnt + 1;

    return 0;
}

static inline int32_t delay_timewheel_tick(
    delay_timewheel_t *tw,
    uint64_t now_cycles,
    struct rte_mbuf **out_pkts,
    uint64_t *out_timestamps,
    uint32_t max_out
) {
    if (unlikely(!tw || !tw->initialized)) {
        return -1;
    }

    uint32_t new_slot = cycles_to_slot(tw, now_cycles);

    uint32_t collected = 0;
    uint32_t slot_idx = tw->current_slot;
    uint32_t slots_visited = 0;

    while (slot_idx != new_slot) {
        uint8_t cnt = tw->slot_counts[slot_idx];

        for (uint8_t j = 0; j < cnt; j++) {
            if (likely(collected < max_out)) {
                out_pkts[collected] = SLOT_PKT(tw, slot_idx, j);
                out_timestamps[collected] = SLOT_TIMESTAMP(tw, slot_idx, j);

                collected++;
            } else {
                // continue counting, so the caller knows we lost packets
                collected++;
                rte_pktmbuf_free(SLOT_PKT(tw, slot_idx, j));
            }
        }

        tw->slot_counts[slot_idx] = 0;
        slot_idx = (slot_idx + 1) & tw->slot_mask;
        slots_visited++;
    }

    tw->current_slot = new_slot;

    return (int32_t)collected;
}

#endif /* DELAY_TIMEWHEEL_H */
