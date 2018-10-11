KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := nvmev.o
nvmev-objs := nvmev_main.o nvmev_pci_init.o nvmev_ops_pci.o nvmev_ops_admin.o nvmev_ops_nvm.o

default:
		$(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules

install:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules_install

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) clean
	   rm -f cscope.out

.PHONY: cscope
cscope:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) cscope

.PHONY: tags
tags:
	   $(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) tags
