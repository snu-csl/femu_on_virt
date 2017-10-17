#!/bin/bash

sudo insmod ./nvmev.ko memmap_start=16 memmap_size=16384 read_latency=6 write_latency=6 read_bw=2000 write_bw=2000 cpu_mask=3
