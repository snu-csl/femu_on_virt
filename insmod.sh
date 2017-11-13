#!/bin/bash

# Core on Node #0
# Storage on Node #0
#sudo insmod ./nvmev.ko memmap_start=64 memmap_size=65536 read_latency=1 write_latency=1 read_bw=2000 write_bw=2000 cpu_mask=20

# Core on Node #1
# Storage on Node #0
#sudo insmod ./nvmev.ko memmap_start=64 memmap_size=65536 read_latency=1 write_latency=1 read_bw=2000 write_bw=2000 cpu_mask=10

# Core on Node #0
# Storage on Node #1
#sudo insmod ./nvmev.ko memmap_start=256 memmap_size=65536 read_latency=1 write_latency=1 read_bw=2000 write_bw=2000 cpu_mask=20

# Core on Node #1
# Storage on Node #1
#sudo insmod ./nvmev.ko memmap_start=256 memmap_size=65536 read_latency=1 write_latency=1 read_bw=2000 write_bw=2000 cpu_mask=10

sudo insmod ./nvmev.ko memmap_start=192 memmap_size=196608 read_latency=1 write_latency=1 read_bw=2000 write_bw=2000 cpu_mask=10

./init_perf.sh
