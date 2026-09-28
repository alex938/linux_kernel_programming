# Build hello.c as a loadable kernel module (hello.ko)
obj-m += hello.o

# Default target: use the running kernel's build tree to compile this directory
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Remove all build artefacts
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
