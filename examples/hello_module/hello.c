// SPDX-License-Identifier: GPL-2.0
/*
 * hello.c - minimal loadable kernel module with a parameter.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>

static int count = 1;
module_param(count, int, 0444);
MODULE_PARM_DESC(count, "Number of greetings to print at load time");

static int __init hello_init(void)
{
	int i;

	if (count < 0 || count > 10)
		return -EINVAL;

	for (i = 0; i < count; i++)
		pr_info("hello: loaded (%d/%d)\n", i + 1, count);
	return 0;
}

static void __exit hello_exit(void)
{
	pr_info("hello: unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal hello-world loadable kernel module");
MODULE_AUTHOR("Advanced Linux Kernel Programming course");
