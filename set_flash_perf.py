#!/usr/bin/env python3
import sys
import os

minimum_latency = [ 6610, 9870 ] # Minimum nanoseconds for read and write
#minimum_latency = [ 0, 0 ] # Minimum nanoseconds for read and write

if len(sys.argv) == 2 and sys.argv[1] == "max":
	delay_initial = [1, 1]
	per_op_latency = [1, 1]
	io_unit_shift = 15
	io_unit_shift = 12

	print("set max")
else:
	if len(sys.argv) < 2:
		print("Usage: %s [Read latency] [Write latency]" % (sys.argv[0]))
		sys.exit(1)

	target_latency = [ float(sys.argv[1]), float(sys.argv[2]) ]

	print(target_latency)

	io_unit_shift = 12
	io_unit_size = 1 << (io_unit_shift - 10)

	# Per-operation flash latency = target - minimum
	per_op_latency = [ (target*1000) - m for (target, m) in zip(target_latency, minimum_latency) ]

	delay_initial = [0, 0]

	print(minimum_latency)
	print(per_op_latency)

os.system("sudo sh -c \"echo %d %d %d > /proc/nvmev/read_times\"" % (delay_initial[0], per_op_latency[0], 0))
os.system("sudo sh -c \"echo %d %d %d > /proc/nvmev/write_times\"" % (delay_initial[1], per_op_latency[1], 0))
os.system("sudo sh -c \"echo %d %d > /proc/nvmev/io_units\"" % (1, io_unit_shift))
