#!/usr/bin/env python3

import csv

STEP_MS = 10

lines = []


def flat(duration, latency_ms, rate_mbps, loss, limit, route_id):
    steps = max(1, round(duration / (STEP_MS / 1000)))
    for _ in range(steps):
        lines.append((round(latency_ms * 1000), round(rate_mbps * 1e6), loss, limit, route_id))


def ramp(duration, start, end):
    steps = max(2, round(duration / (STEP_MS / 1000)))
    for i in range(steps):
        t = i / (steps - 1)
        lines.append((
            round((start[0] + (end[0] - start[0]) * t) * 1000),
            round((start[1] + (end[1] - start[1]) * t) * 1e6),
            start[2] + (end[2] - start[2]) * t,
            round(start[3] + (end[3] - start[3]) * t),
            round(start[4] + (end[4] - start[4]) * t),
        ))


def write(path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        for lat, rate, loss, limit, rid in lines:
            w.writerow([lat, rate, f"{loss:.2f}", limit, rid])


# build your trace here

flat(5.0, 15, 300, 0, 1000, 1)
flat(0.1, 0, 0, 0, 1000, 2)
flat(5.0, 15, 300, 0, 1000, 2)

write("trace_return.csv")
lines.clear()

flat(5.0, 10, 100, 0, 1000, 1)
flat(0.1, 0, 0, 0, 1000, 2)
flat(5.0, 10, 100, 0, 1000, 2)

write("trace_forward.csv")