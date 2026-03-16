#ifndef TRACE_TIMEWHEEL_H
#define TRACE_TIMEWHEEL_H

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <stdbool.h>
#include <stdint.h>

#define TIMEWHEEL_TICK_INTERVAL_MS 10
#define FP_SHIFT 16
#define FP_SHIFT_48 48

typedef struct {
    uint64_t cycles_per_byte_fp;
    uint64_t delay_us;
    uint16_t queue_cap;
    uint64_t loss_threshold;
    uint16_t route_id;
} __rte_cache_aligned network_params_t;

typedef struct {
    network_params_t *entries;
    uint32_t size;
    uint32_t mask;
    uint64_t ticks_per_cycle_fp48;
    uint64_t start_cycles;
} __rte_cache_aligned trace_timewheel_t;

trace_timewheel_t *trace_timewheel_create(const char *csv_file);
void trace_timewheel_destroy(trace_timewheel_t *tw);

static inline void
trace_timewheel_set_start_cycles(trace_timewheel_t *tw, uint64_t start_cycles) {
    if (likely(tw)) {
        tw->start_cycles = start_cycles;
    }
}

// Convert elapsed cycles since start to a tick index, then mask into
// the power-of-2 entry array. Each tick is TIMEWHEEL_TICK_INTERVAL_MS.
static inline network_params_t *
trace_timewheel_get_params(trace_timewheel_t *tw, uint64_t current_cycles) {
    if (unlikely(!tw)) {
        return NULL;
    }

    //clang-format off
    uint32_t ticks =
        (uint32_t)(((__uint128_t)(current_cycles - tw->start_cycles) *
        tw->ticks_per_cycle_fp48) >> FP_SHIFT_48);
    //clang-format on

    return &tw->entries[ticks & tw->mask];
}

#endif
