# Build kthread_demo.c as a loadable kernel module (kthread_demo.ko)
obj-m += kthread_demo.o

# Default target: use the running kernel's build tree to compile this directory
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Remove all build artefacts
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
