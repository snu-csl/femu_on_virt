# NVMeVirt

### Introduction

- Recommend kernel v4.4.x (tested on 4.4.108)

- The memory to emulate NVMe device should be reserved. To reserve a chunk of memory, add the following options to `GRUB_CMDLINE_LINUX` in

  `/etc/default/grub` as follow:

  ```bash
  GRUB_CMDLINE_LINUX="memmap=64G\\\$128G"
  ```

  This option will reserve 64GB of memory from the 128GB memory offset.

### Run

- The kernel module for NVMe should be unloaded before loading nvmev.ko

- Load the module

  ```bash
  $ sudo insmod ./nvmev.ko \
      memmap_start=128  \ # in GiB
      memmap_size=65536 \ # in MiB
      cpus=1,3,17,19    \ # CPU lists for process completions
      read_latency=1000 \
      write_latency=1000 \
      read_bw=2000 \
      write_bw=2000
  ```

- (CAUTION) The current setting up procedure does not work properly (sometimes sets up the system counter-intuitively). To workaround the buggy setup, load the module with some arbitrary parameters, and later set read/write latency and the parallelism degree explicitly as follow:

  ```bash
  $ sudo sh -c 'echo 1  > /proc/nvmev/read_latency'
  $ sudo sh -c 'echo 1  > /proc/nvmev/write_latency'
  $ sudo sh -c 'echo 32 > /proc/nvmev/slot'
  ```

  With the `slot` of internal parallelism degree, the bandwidth can be calculated as:

  Bandwidth (Bytes/S) = slot * 4KB / (latency in second)
