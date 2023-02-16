KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := nvmev.o
nvmev-objs := main.o pci.o admin.o io.o 
nvmev-objs += ssd.o conv_ftl.o pqueue.o dma.o
nvmev-objs += zns_ftl.o zns_read_write.o zns_mgmt_send.o zns_mgmt_recv.o 
nvmev-objs += channel_model.o 

ccflags-y += -Wno-implicit-fallthrough -Wno-unused-function -Wno-declaration-after-statement -Wno-unused-variable
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
