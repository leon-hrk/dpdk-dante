#include "forwarder.h"
#include "delay_timewheel.h"
#include "logging.h"
#include "trace_timewheel.h"
#include "vqueue_ring.h"

#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <stdbool.h>

#define RTE_LOGTYPE_FORWARD RTE_LOGTYPE_USER1

//clang-format off
static inline uint64_t acquire_shared_start(uint64_t *shared, uint64_t now) {
    uint64_t expected = 0;
    if (__atomic_compare_exchange_n(
            shared, &expected, now, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST
    )) {
        return now;
    }
    return expected;
}
//clang-format on

static inline uint64_t
calculate_bandwidth_delay(uint64_t total_bytes, uint64_t cycles_per_byte_fp) {
    return (total_bytes * cycles_per_byte_fp) >> FP_SHIFT;
}

static inline uint64_t fast_random(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static inline bool try_set_start_cycles(
    struct forward_params *params,
    delay_timewheel_t *delay_tw,
    trace_timewheel_t *trace_tw,
    bool had_rx
) {
    uint64_t start;
    if (had_rx) {
        start = acquire_shared_start(params->shared_start, rte_rdtsc());
    } else {
        start = __atomic_load_n(params->shared_start, __ATOMIC_SEQ_CST);
        if (start == 0) {
            return false;
        }
    }
    delay_timewheel_set_start_cycles(delay_tw, start);
    trace_timewheel_set_start_cycles(trace_tw, start);
    RTE_LOG(
        INFO,
        FORWARD,
        "[%s] start_cycles = %" PRIu64 "\n",
        params->name_suffix,
        start
    );
    return true;
}

int forward_packets(void *arg) {
    struct forward_params *params = (struct forward_params *)arg;

    const uint64_t cycles_per_us = rte_get_tsc_hz() / 1000000ULL;

    const uint64_t us_per_cycle_fp32 =
        ((uint64_t)1 << 32) / (rte_get_tsc_hz() / 1000000UL);
    uint64_t rng_state = rte_rdtsc() ^ rte_lcore_id();

    worker_stats_t stats;
    worker_stats_reset(&stats);

    delay_timewheel_t *delay_tw = delay_timewheel_create();
    if (!delay_tw) {
        return -1;
    }

    vqueue_ring_t *queue_ring = vqueue_ring_create(rte_socket_id());
    if (!queue_ring) {
        delay_timewheel_destroy(delay_tw);
        return -1;
    }

    trace_timewheel_t *trace_tw = trace_timewheel_create(params->csv_file);
    if (!trace_tw) {
        vqueue_ring_destroy(queue_ring);
        delay_timewheel_destroy(delay_tw);
        return -1;
    }

    struct rte_mbuf *rx_mbufs[MAX_QUEUE_CAP];
    struct rte_mbuf *tx_mbufs[TX_RING_SIZE];
    uint64_t timestamps[TX_RING_SIZE];

    uint16_t queue_size = 0;
    struct rte_mbuf *bw_pkt = NULL;
    uint64_t bw_pkt_rx_cycles = 0;
    uint64_t bw_send_time = 0;
    bool bw_zero = false;
    bool start_set = false;

    // Packet flow: RX -> vqueue_ring_t_ring (queue) -> bandwidth pacer ->
    // delay_timewheel -> TX
    //
    // On each iteration:
    //
    // Stage 1: Receive packets, enqueue up to queue_cap
    // Stage 2: Collect delay-expired packets from timewheel and transmit
    // Stage 3: Drain queue through bandwidth pacer into delay timewheel
    while (!*(params->stop)) {
        uint64_t now = rte_rdtsc();
        network_params_t *net = trace_timewheel_get_params(trace_tw, now);

        // Stage 1: RX and queue admission
        uint16_t n_rx =
            rte_eth_rx_burst(params->rx_port, 0, rx_mbufs, RX_RING_SIZE);

        if (unlikely(!start_set)) {
            start_set =
                try_set_start_cycles(params, delay_tw, trace_tw, n_rx > 0);
            if (!start_set) {
                continue;
            }
        }

        if (n_rx > 0) {
            uint16_t space = 0;
            if (queue_size < net->queue_cap) {
                space = net->queue_cap - queue_size;
            }

            uint16_t n_accept = (n_rx < space) ? n_rx : space;
            if (n_accept > 0) {
                uint16_t n_enq = vqueue_ring_enqueue_burst(
                    queue_ring, rx_mbufs, n_accept, now
                );
                if (unlikely(n_enq < n_accept)) {
                    stats.tx_dropped_unexpected += n_accept - n_enq;
                    for (uint16_t i = n_enq; i < n_accept; i++) {
                        rte_pktmbuf_free(rx_mbufs[i]);
                    }
                }
                queue_size += n_enq;
                stats.rx_success += n_enq;
            }

            for (uint16_t i = n_accept; i < n_rx; i++) {
                rte_pktmbuf_free(rx_mbufs[i]);
            }
            stats.rx_dropped += (n_rx - n_accept);
        }

        // Stage 2: Collect delay-expired packets and TX
        int32_t n_collected = delay_timewheel_tick(
            delay_tw, now, tx_mbufs, timestamps, TX_RING_SIZE
        );

        if (n_collected > 0) {
            uint16_t n_valid = (n_collected > TX_RING_SIZE)
                                   ? TX_RING_SIZE
                                   : (uint16_t)n_collected;

            if (unlikely(n_collected > TX_RING_SIZE)) {
                stats.tx_dropped_unexpected += n_collected - TX_RING_SIZE;
            }

            for (uint16_t j = 0; j < n_valid; j++) {
                uint64_t delay_us =
                    cycles_to_us(now - timestamps[j], us_per_cycle_fp32);
                if (likely(delay_us < LATENCY_HISTOGRAM_SIZE)) {
                    stats.latency_histogram[delay_us]++;
                } else {
                    stats.latency_histogram[LATENCY_HISTOGRAM_SIZE - 1]++;
                }
            }

            uint16_t n_sent =
                rte_eth_tx_burst(params->tx_port, 0, tx_mbufs, n_valid);
            stats.tx_success += n_sent;

            if (unlikely(n_sent < n_valid)) {
                stats.tx_dropped += n_valid - n_sent;
                for (uint16_t j = n_sent; j < n_valid; j++) {
                    rte_pktmbuf_free(tx_mbufs[j]);
                }
            }
        }

        // Stage 3: Bandwidth pacing into the delay timewheel

        // UINT64_MAX signals zero bandwidth — hold all packets
        if (net->cycles_per_byte_fp == UINT64_MAX) {
            bw_zero = true;
            continue;
        }

        if (bw_pkt == NULL) {
            if (vqueue_ring_dequeue(queue_ring, &bw_pkt, &bw_pkt_rx_cycles) ==
                0) {
                uint32_t total_bytes = rte_pktmbuf_pkt_len(bw_pkt);
                // After a zero-bandwidth period, re-anchor pacing to now
                // rather than using stale timestamps
                if (bw_zero) {
                    bw_send_time =
                        now + calculate_bandwidth_delay(
                                  total_bytes, net->cycles_per_byte_fp
                              );
                    bw_zero = false;
                } else {
                    bw_send_time = bw_pkt_rx_cycles +
                                   calculate_bandwidth_delay(
                                       total_bytes, net->cycles_per_byte_fp
                                   );
                }
            }
        }

        if (bw_pkt == NULL || now < bw_send_time) {
            continue;
        }

        // Re-anchor after zero-bandwidth period for a packet that was
        // already dequeued before bandwidth dropped to zero
        if (bw_zero) {
            uint32_t total_bytes = rte_pktmbuf_pkt_len(bw_pkt);
            bw_send_time =
                now +
                calculate_bandwidth_delay(total_bytes, net->cycles_per_byte_fp);
            bw_zero = false;
            continue;
        }

        uint64_t delay_cycles = (uint64_t)net->delay_us * cycles_per_us;

        // Drain queued packets into the delay wheel as long as their
        // bandwidth-paced send time has already passed. Each packet's
        // send time is max(arrival, prev_send_time) to model a serializing
        // link.
        do {
            queue_size--;
            if (net->loss_threshold &&
                fast_random(&rng_state) < net->loss_threshold) {
                rte_pktmbuf_free(bw_pkt);
                stats.tx_dropped++;
            } else {
                int n_inserted = delay_timewheel_insert_at(
                    delay_tw,
                    bw_pkt,
                    bw_send_time + delay_cycles,
                    bw_pkt_rx_cycles + delay_cycles,
                    net->route_id
                );
                if (n_inserted == -1) {
                    stats.tx_dropped_unexpected++;
                    rte_pktmbuf_free(bw_pkt);
                    bw_pkt = NULL;
                    break;
                }
            }

            if (vqueue_ring_dequeue(queue_ring, &bw_pkt, &bw_pkt_rx_cycles) !=
                0) {
                bw_pkt = NULL;
                break;
            }

            uint32_t total_bytes = rte_pktmbuf_pkt_len(bw_pkt);
            // If this packet arrived after the previous one finished sending,
            // the link was idle. Pace from arrival time.
            // Otherwise, pace from end of previous transmission.
            if (bw_pkt_rx_cycles > bw_send_time) {
                bw_send_time =
                    bw_pkt_rx_cycles + calculate_bandwidth_delay(
                                           total_bytes, net->cycles_per_byte_fp
                                       );
            } else {
                bw_send_time += calculate_bandwidth_delay(
                    total_bytes, net->cycles_per_byte_fp
                );
            }
        } while (bw_send_time <= now);
    }

    worker_stats_print(params->name_suffix, &stats);
    worker_stats_save(&stats, params->stats_file);

    // Cleanup: drain remaining packets
    struct rte_mbuf *remaining_pkt;
    while (vqueue_ring_dequeue(queue_ring, &remaining_pkt, &bw_pkt_rx_cycles) ==
           0) {
        rte_pktmbuf_free(remaining_pkt);
    }

    if (bw_pkt != NULL) {
        rte_pktmbuf_free(bw_pkt);
    }

    trace_timewheel_destroy(trace_tw);
    vqueue_ring_destroy(queue_ring);
    delay_timewheel_destroy(delay_tw);

    return 0;
}
