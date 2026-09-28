// SPDX-License-Identifier: GPL-2.0
/* ^ SPDX tag: machine-readable licence of this file (required in kernel sources) */
/*
 * hello.c - minimal loadable kernel module with a parameter.
 */
#include <linux/init.h>		/* __init / __exit section markers */
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() macros */
#include <linux/moduleparam.h>	/* module_param(), MODULE_PARM_DESC() */
#include <linux/printk.h>	/* pr_info() logging to the kernel ring buffer */
static int count = 1;		/* module parameter: how many greetings; default 1 */
module_param(count, int, 0444);	/* expose 'count' as an int param, read-only in /sys/module/hello/parameters/ */
MODULE_PARM_DESC(count, "Number of greetings to print at load time"); /* description shown by modinfo (parm=) */
static int __init hello_init(void)	/* runs once at insmod; __init code is freed after loading */
{					/* start of hello_init() */
	int i;				/* loop counter */
	if (count < 0 || count > 10)	/* reject out-of-range parameter values */
		return -EINVAL;		/* non-zero return = load fails, module is not inserted */
	for (i = 0; i < count; i++)	/* repeat 'count' times */
		pr_info("hello: loaded (%d/%d)\n", i + 1, count); /* log a KERN_INFO message (see dmesg) */
	return 0;			/* 0 = success, module becomes "Live" */
}					/* end of hello_init() */
static void __exit hello_exit(void)	/* runs at rmmod; __exit code is dropped if built in */
{					/* start of hello_exit() */
	pr_info("hello: unloaded\n");	/* log that the module is being removed */
}					/* end of hello_exit(): no return value, cannot fail */
module_init(hello_init);		/* register hello_init() as the module's entry point */
module_exit(hello_exit);		/* register hello_exit() as the module's exit point */
MODULE_LICENSE("GPL");			/* GPL-compatible: allows GPL-only symbols, no taint */
MODULE_DESCRIPTION("Minimal hello-world loadable kernel module"); /* shown by modinfo */
MODULE_AUTHOR("Advanced Linux Kernel Programming course");	/* shown by modinfo */
