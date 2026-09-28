# Makefile: builds kthread_demo.c into the loadable kernel module kthread_demo.ko
obj-m := kthread_demo.o	# tell Kbuild to compile kthread_demo.c and link it as kthread_demo.ko
KDIR ?= /lib/modules/$(shell uname -r)/build	# kernel build tree: headers of the running kernel
all:	# default target, run by a plain "make"
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules	# enter the kernel tree (-C) and build the module in this dir (M=)
clean:	# target run by "make clean"
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean	# let Kbuild delete all generated files (.ko, .o, .mod.c, ...)
