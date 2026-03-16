#include "forwarder.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_timer.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define N_PORTS 2
#define N_MBUFS 4194304 // ~10GB of space for packets
#define CACHE_SIZE 256

#define DIE_ON_ERR(rc, fmt, ...)                                               \
    do {                                                                       \
        if ((rc) < 0)                                                          \
            rte_exit(                                                          \
                EXIT_FAILURE, fmt ": %s\n", ##__VA_ARGS__, rte_strerror(-(rc)) \
            );                                                                 \
    } while (0)

struct dante_context {
    volatile int stop_flag;
    uint64_t shared_start; // TSC timestamp for synced trace start
    struct rte_mempool *mbuf_pool;
    struct forward_params params_fwd;
    struct forward_params params_ret;
    unsigned int lcore_fwd;
    unsigned int lcore_ret;
};

// clang-format off
static struct dante_context context = {
    .stop_flag = 0,
    .shared_start = 0,
    .mbuf_pool = NULL,
    .params_fwd = {
            .rx_port = 0,
            .tx_port = 1,
            .name_suffix = "forward",
            .csv_file = "trace_forward.csv",
            .stats_file = "/tmp/forward_stats.txt",
    },
    .params_ret = {
        .rx_port = 1,
        .tx_port = 0,
        .name_suffix = "return",
        .csv_file = "trace_return.csv",
        .stats_file = "/tmp/return_stats.txt",
    },
};
// clang-format on

static void signal_handler(int signum __rte_unused) {
    context.stop_flag = 1;
}

static void init_port(uint16_t portid, struct rte_mempool *pool) {
    struct rte_eth_conf port_conf = {
        .rxmode = {.mq_mode = RTE_ETH_MQ_RX_NONE},
        .txmode = {.mq_mode = RTE_ETH_MQ_TX_NONE},
    };
    int sid = rte_eth_dev_socket_id(portid);
    int rc;

    rc = rte_eth_dev_configure(portid, 1, 1, &port_conf);
    DIE_ON_ERR(rc, "port %u configure", portid);

    rc = rte_eth_rx_queue_setup(portid, 0, RX_RING_SIZE, sid, NULL, pool);
    DIE_ON_ERR(rc, "port %u rx setup", portid);

    rc = rte_eth_tx_queue_setup(portid, 0, TX_RING_SIZE, sid, NULL);
    DIE_ON_ERR(rc, "port %u tx setup", portid);

    rc = rte_eth_dev_start(portid);
    DIE_ON_ERR(rc, "port %u start", portid);

    rc = rte_eth_promiscuous_enable(portid);
    DIE_ON_ERR(rc, "port %u promisc", portid);

    printf("port %u ready\n", portid);
}

static int running(void) {
    return rte_eal_get_lcore_state(context.lcore_fwd) == RUNNING ||
           rte_eal_get_lcore_state(context.lcore_ret) == RUNNING;
}

static void start_workers(void) {
    context.stop_flag = 0;
    context.shared_start = 0;

    context.params_fwd.stop = &context.stop_flag;
    context.params_fwd.shared_start = &context.shared_start;
    context.params_ret.stop = &context.stop_flag;
    context.params_ret.shared_start = &context.shared_start;

    rte_eal_remote_launch(
        forward_packets, &context.params_ret, context.lcore_ret
    );
    rte_eal_remote_launch(
        forward_packets, &context.params_fwd, context.lcore_fwd
    );
}

static void stop_workers(void) {
    context.stop_flag = 1;
    rte_eal_wait_lcore(context.lcore_fwd);
    rte_eal_wait_lcore(context.lcore_ret);
}

static void console(void) {
    char line[512], dir[64], field[64], path[CSV_PATH_MAX];

    for (;;) {
        printf("dante> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) {
            continue;
        }

        if (!strcmp(line, "quit")) {
            break;
        } else if (!strcmp(line, "start")) {
            if (running()) {
                printf("already running\n");
            } else {
                start_workers();
                printf("started\n");
            }
        } else if (!strcmp(line, "stop")) {
            if (!running()) {
                printf("already stopped\n");
            } else {
                stop_workers();
                printf("stopped\n");
            }
        } else if (sscanf(line, "set %63s %63s %255s", dir, field, path) == 3) {
            if (running()) {
                printf("stop first\n");
                continue;
            }

            struct forward_params *p = NULL;
            if (!strcmp(dir, "fwd")) {
                p = &context.params_fwd;
            } else if (!strcmp(dir, "ret")) {
                p = &context.params_ret;
            }

            if (!p) {
                printf("bad direction '%s'\n", dir);
            } else if (!strcmp(field, "trace")) {
                snprintf(p->csv_file, CSV_PATH_MAX, "%s", path);
                printf("  -> %s\n", p->csv_file);
            } else if (!strcmp(field, "stats")) {
                snprintf(p->stats_file, CSV_PATH_MAX, "%s", path);
                printf("  -> %s\n", p->stats_file);
            } else {
                printf("bad field '%s'\n", field);
            }
        } else {
            printf(
                "bad command: start, stop, set <fwd|ret> <trace|stats> <path>, "
                "quit\n"
            );
        }
    }

    if (running()) {
        stop_workers();
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int rc = rte_eal_init(argc, argv);
    if (rc < 0) {
        rte_exit(EXIT_FAILURE, "eal init failed\n");
    }
    rte_log_set_global_level(RTE_LOG_WARNING);

    if (rte_eth_dev_count_avail() != N_PORTS) {
        rte_exit(EXIT_FAILURE, "need exactly %u ports\n", N_PORTS);
    }

    rc = rte_timer_subsystem_init();
    DIE_ON_ERR(rc, "timer subsystem init failed");

    context.mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        N_MBUFS,
        CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id()
    );
    if (!context.mbuf_pool) {
        rte_exit(EXIT_FAILURE, "mbuf pool alloc failed\n");
    }

    for (uint16_t p = 0; p < N_PORTS; p++) {
        init_port(p, context.mbuf_pool);
    }

    // Skip the main lcore (used for the console), get the two worker lcores.
    context.lcore_ret = rte_get_next_lcore((unsigned int)-1, 1, 0);
    context.lcore_fwd = rte_get_next_lcore(context.lcore_ret, 1, 0);

    if (context.lcore_ret >= RTE_MAX_LCORE ||
        context.lcore_fwd >= RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "need 3+ lcores\n");
    }
    if (context.lcore_ret == context.lcore_fwd) {
        rte_exit(EXIT_FAILURE, "fwd/ret need distinct lcores\n");
    }

    console();

    for (uint16_t p = 0; p < N_PORTS; p++) {
        rte_eth_dev_stop(p);
        rte_eth_dev_close(p);
    }

    rte_eal_cleanup();
    return 0;
}
