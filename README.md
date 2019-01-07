# NVMeVirt

### Introduction

- Recommend kernel v4.4.x (tested on 4.4.108)

- A part of main memory should be reserved to emulate NVMe device. To reserve a chunk of memory, add the following options to `GRUB_CMDLINE_LINUX` in

  `/etc/default/grub` as follow:

  ```bash
  GRUB_CMDLINE_LINUX="memmap=64G\\\$128G"
  ```

  This option will reserve 64GB of memory chunk starting from 128GB memory offset.

### Run

- Make sure the kernel module for NVMe (`nvme.ko`) should be unloaded before loading the NVMeVirt module.

- Compile. Make sure the kernel source in the Makefile is what you want to use

  ```bash
  $ make
  make -C /lib/modules/4.4.108/build SUBDIRS=/path/to/nvmev modules
  make[1]: Entering directory `/path/to/linux-4.4.108'
    CC [M]  /path/to/nvmev/nvmev_main.o
    CC [M]  /path/to/nvmev/nvmev_pci_init.o
    CC [M]  /path/to/nvmev/nvmev_ops_pci.o
    CC [M]  /path/to/nvmev/nvmev_ops_admin.o
    CC [M]  /path/to/nvmev/nvmev_ops_nvm.o
    LD [M]  /path/to/nvmev/nvmev.o
    Building modules, stage 2.
    MODPOST 1 modules
    CC      /path/to/nvmev/nvmev.mod.o
    LD [M]  /path/to/nvmev/nvmev.ko
  make[1]: Leaving directory `/path/to/linux-4.4.108'
  $
  ```

- Load the module.

  ```bash
  $ sudo insmod ./nvmev.ko \
      memmap_start=128  \ # in GiB
      memmap_size=65536 \ # in MiB
      cpus=1,3,17,19    \ # List of CPUs to process I/O requests (should have at least 2)
      read_latency=1000 \ # in usec
      write_latency=1000 \ # in usec
      read_bw=2000 \ # Target bandwidth in MiB/sec
      write_bw=2000  # Target bandwidth in MiB/sec
  ```

- (CAUTION) The current setting up procedure does not work properly (sometimes sets up the system counter-intuitively). To workaround the buggy setup, load the module with some arbitrary parameters, and later set read/write latency and the parallelism degree explicitly as follow:

  ```bash
  $ sudo sh -c 'echo 1  > /proc/nvmev/read_latency'
  $ sudo sh -c 'echo 1  > /proc/nvmev/write_latency'
  $ sudo sh -c 'echo 32 > /proc/nvmev/slot'
  ```

  With the `slot` of internal parallelism degree, the bandwidth can be calculated as:

  Bandwidth (Bytes/sec) = slot * 4KB / (latency in second)
