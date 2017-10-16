#!/bin/bash

sudo insmod ./nvmev.ko memmap_start=30 memmap_size=2048 read_latency=1 write_latency=2 read_bw=3 write_bw=4 cpu_mask=3
