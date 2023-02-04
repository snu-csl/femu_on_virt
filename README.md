# NVMeVirt

### Introduction

- Recommend kernel v4.4.x (tested on 4.4.108)

- A part of main memory should be reserved to emulate NVMe device. To reserve a pg of memory, add the following options to `GRUB_CMDLINE_LINUX` in

  `/etc/default/grub` as follow:

  ```bash
  GRUB_CMDLINE_LINUX="memmap=64G\\\$128G"
  ```

  This option will reserve 64GB of memory page starting from 128GB memory offset.

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
			io_unit_size=4096 \ # I/O unit
			nr_io_units=8			  # Number of I/O units that can be accessed in parallel
  ```

- With the `nr_io_units` of internal parallel I/O units, the bandwidth can be calculated as:

  Bandwidth (bytes/sec) = `nr_io_units` * `io_unit_size` / (latency in second)
