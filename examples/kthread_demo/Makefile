# Kbuild: build kthread_demo.c into the loadable module kthread_demo.ko
obj-m += kthread_demo.o

# Kernel build tree to build against; defaults to the running kernel's headers
KDIR ?= /lib/modules/$(shell uname -r)/build

# Default target: invoke the kernel's build system for this external module
all:
# -C: run make in the kernel tree; M=: location of this module's sources
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# Remove all build artefacts (.o, .ko, .mod.c, ...)
clean:
# Let the kernel build system clean this module directory
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
