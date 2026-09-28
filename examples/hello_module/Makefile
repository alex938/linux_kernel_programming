# Makefile: builds hello.c into the loadable kernel module hello.ko
obj-m := hello.o	# tell Kbuild to compile hello.c and link it as hello.ko
KDIR ?= /lib/modules/$(shell uname -r)/build	# kernel build tree: headers of the running kernel
all:	# default target, run by a plain "make"
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules	# enter the kernel tree (-C) and build the module in this dir (M=)
clean:	# target run by "make clean"
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean	# let Kbuild delete all generated files (.ko, .o, .mod.c, ...)
