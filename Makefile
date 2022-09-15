KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := nvmev.o
nvmev-objs := main.o pci.o admin.o io.o ftl.o pqueue.o dma.o
nvmev-objs += zns.o zone_read_write.o zone_mgmt_send.o zone_mgmt_recv.o 
nvmev-objs += channel.o

default:
		$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
		ctags -R

install:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags
	   rm -f io.o.ur-safe .cache.mk

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]
