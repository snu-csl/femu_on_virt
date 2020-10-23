KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := nvmev.o
nvmev-objs := main.o pci.o admin.o io.o

default:
		$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]
