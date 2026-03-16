# DANTE

A DPDK-based layer-2 network emulator that applies configurable delay, bandwidth limiting, packet loss, and queuing. Driven by a trace file that can change parameters over time. Each direction (forward and return) runs on its own lcore and reads its own trace file independently. Route IDs enforce in-order delivery for packets sharing the same route, as expected in satellite networks where path delays can change significantly in short time periods. On DPDK-capable hardware, throughput beyond 20 Gbps is expected; the provided [virtual setup](#virtual-setup-vhost-user--qemu) is limited to roughly 1 Gbps due to VM overhead. This project is inspired by [TheaterQ](https://github.com/cs7org/TheaterQ) and [PhantomLink](https://github.com/robinohs/phantomlink).

## Setup & Build

Allocate 20 GB of hugepages via GRUB:

```
GRUB_CMDLINE_LINUX="default_hugepagesz=1G hugepagesz=1G hugepages=20"
```

Create a mount point:

```bash
sudo mkdir -p /dev/hugepages-1G
sudo mount -t hugetlbfs -o pagesize=1G none /dev/hugepages-1G
```

The two worker lcores must be isolated from the kernel scheduler and must be physical cores, not hyperthreads. Add to GRUB:

```
GRUB_CMDLINE_LINUX="isolcpus=managed_irq,domain,2,3 nohz_full=2,3 rcu_nocbs=2,3 rcu_nocb_poll irqaffinity=0"
```

Adjust `2,3` to match the lcores later passed to `-l`.

After editing `/etc/default/grub`:

```bash
sudo update-grub
sudo reboot
```

Build with `make` (requires [DPDK](https://doc.dpdk.org/guides/prog_guide/build-sdk-meson.html), tested with 26.03).


## Usage

```
sudo ./dpdk-dante -l 0,2,3 -n 2 [EAL options]
```

Adapt `-l` to your main + two isolated worker cores, `-n` to the number of memory channels, and add further EAL options for devices or hugepage configuration as needed. See [Virtual Setup](#virtual-setup) for a full example.

Commands:

- `start` — launch both forwarding workers
- `stop` — stop both workers and write stats
- `set <fwd|ret> trace <path>` — set trace file (while stopped)
- `set <fwd|ret> stats <path>` — set stats output path (while stopped)
- `quit` — stop workers and exit

Defaults:
- Trace files: `trace_forward.csv` and `trace_return.csv` in the working directory
- Stats files: `/tmp/forward_stats.txt` and `/tmp/return_stats.txt`

## Trace File Format

```
<LATENCY>,<RATE>,<LOSS>,<LIMIT>,<ROUTE_ID>\n
   u64     u64     a)      b)       c)

# a) float: packet loss in percent (0.0 - 100.0)
# b) u16: queue capacity, must be < 4096
# c) u16: route identifier, must be < 512
```

- `LATENCY` — Per-packet delay in microseconds.
- `RATE` — Bandwidth limitation in bit/s, achieved through packet spacing. A value of 0 means no packets leave the queue.
- `LOSS` — Probability of packet loss in percent.
- `LIMIT` — Queue capacity (in packets) before the bandwidth limiter.
- `ROUTE_ID` — Prevents packet reordering caused by high delay variability. Packets with the same route ID are guaranteed to maintain their order. Use 0 to allow generic reordering.

One line per time step. Each line is held for a fixed interval of 10ms. Both trace files start replay simultaneously when the first packet arrives at either interface. The trace is padded by repeating the last value until the next power of 2 size and then played in a loop.

You can use the provided `tracegen.py` script to generate trace files. Edit the script to fit your own needs. By default, it creates traces that mimic a Starlink reconfiguration event

## Stats Output

Each worker writes a stats file on stop (default `/tmp/forward_stats.txt` and `/tmp/return_stats.txt`):

```
rx_success: 1000000
rx_dropped: 0
tx_success: 1000000
tx_dropped: 0
tx_dropped_unexpected: 0
---
0us: 999401
1us: 517
2us: 92
```

- `rx_dropped` — packets that arrived but were dropped because the emulated queue was full
- `tx_dropped` — packets dropped due to emulated packet loss
- `tx_dropped_unexpected` — packets lost within the emulator due to configuration or performance issues

The histogram section shows overhead latency in microseconds, computed as `tx_timestamp - rx_timestamp - bandwidth_delay - configured_delay`. This captures queueing delay, additional delay from route ordering, and any processing overhead introduced by the emulator itself.

## Virtual Setup (vhost-user + QEMU)

This setup uses a single VM with two network namespaces acting as client and server. This allows capturing traffic on both sides with matching timestamps for evaluation purposes.

Launch dante with vhost-user ports:

```
sudo ./dpdk-dante -l 0,2,3 -n 2 \
    --vdev 'net_vhost0,iface=/tmp/sock-client,queues=1' \
    --vdev 'net_vhost1,iface=/tmp/sock-server,queues=1'
```

Start an Ubuntu Server VM with one management NIC and two vhost-user NICs:

```
sudo qemu-system-x86_64 \
    -name "ubuntu-vm" \
    -enable-kvm -cpu host -m 8G -smp 6 \
    -object memory-backend-file,id=mem,size=8G,mem-path=/dev/hugepages-1G,share=on \
    -numa node,memdev=mem \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:00:01:01 \
    -chardev socket,id=chr1,path=/tmp/sock-client \
    -netdev vhost-user,id=net1,chardev=chr1,vhostforce=on,queues=1 \
    -device virtio-net-pci,netdev=net1,mac=52:54:00:00:01:02,rx_queue_size=1024,tx_queue_size=1024 \
    -chardev socket,id=chr2,path=/tmp/sock-server \
    -netdev vhost-user,id=net2,chardev=chr2,vhostforce=on,queues=1 \
    -device virtio-net-pci,netdev=net2,mac=52:54:00:00:01:03,rx_queue_size=1024,tx_queue_size=1024 \
    -hda "ubuntu.img" -nographic
```

Connect to the VM via SSH:

```
ssh -p 2222 <user>@localhost
```

Inside the VM, isolate the two NICs into separate network namespaces:

```bash
sudo ip netns add client
sudo ip netns add server

sudo ip link set ens4 netns client
sudo ip netns exec client ip link set lo up
sudo ip netns exec client ip addr add 10.0.0.1/24 dev ens4
sudo ip netns exec client ip link set ens4 up

sudo ip link set ens5 netns server
sudo ip netns exec server ip link set lo up
sudo ip netns exec server ip addr add 10.0.0.2/24 dev ens5
sudo ip netns exec server ip link set ens5 up
```

Traffic between `client` (10.0.0.1) and `server` (10.0.0.2) now passes through dante. The trace replay starts on the first received packet, and vhost-user interfaces may generate management traffic that triggers it early. Use `--log-level=7` to track when the trace replay starts.

Test with ping:

```bash
sudo ip netns exec client ping 10.0.0.2
```

Test throughput with iperf3:

```bash
sudo ip netns exec server iperf3 -s
```
```bash
sudo ip netns exec client iperf3 -c 10.0.0.2
```

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).