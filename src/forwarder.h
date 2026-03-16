#ifndef FORWARDER_H
#define FORWARDER_H

#include <stdint.h>

#define CSV_PATH_MAX 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

struct forward_params {
    uint16_t rx_port;
    uint16_t tx_port;
    const char *name_suffix;
    char csv_file[CSV_PATH_MAX];
    char stats_file[CSV_PATH_MAX];
    volatile int *stop;
    uint64_t *shared_start;
};

int forward_packets(void *arg);

#endif /* FORWARDER_H */
