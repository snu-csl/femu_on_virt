KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := nvmev.o
nvmev-objs := nvmev_main.o nvmev_ops_pci.o nvmev_ops_adminq.o nvmev_ops_ioq.o

default:
		$(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules

install:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules_install

clean:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) clean
