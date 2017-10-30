#!/bin/bash

sudo insmod ./nvmev.ko memmap_start=16 memmap_size=16384 read_latency=7 write_latency=7 read_bw=2000 write_bw=2000 cpu_mask=3
