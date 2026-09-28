// SPDX-License-Identifier: GPL-2.0
/* ^ SPDX tag: machine-readable licence of this file (required in kernel sources) */
/*
 * kthread_demo.c - start a kernel thread at load, stop it at unload.
 */
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() macros */
#include <linux/kthread.h>	/* kthread_run(), kthread_stop(), kthread_should_stop() */
#include <linux/delay.h>	/* msleep_interruptible() */
#include <linux/sched.h>	/* current, struct task_struct */
static struct task_struct *worker;	/* handle to our kernel thread (NULL until started) */
static int worker_fn(void *data)	/* body of the kernel thread; 'data' is the arg from kthread_run() */
{					/* start of worker_fn() */
	while (!kthread_should_stop()) {	/* loop until kthread_stop() is called on us */
		pr_info("kthread demo: pid=%d mm=%p\n", current->pid, current->mm); /* mm is NULL: no user address space */
		msleep_interruptible(5000);	/* sleep ~5 s; wakes early when kthread_stop() wakes us */
	}				/* end of loop: a stop was requested */
	return 0;			/* exit code handed back to kthread_stop() */
}					/* end of worker_fn() */
static int __init kthread_demo_init(void)	/* runs at insmod */
{						/* start of kthread_demo_init() */
	worker = kthread_run(worker_fn, NULL, "kthread_demo"); /* create (via kthreadd) and wake the thread */
	if (IS_ERR(worker))			/* kthread_run() returns an ERR_PTR on failure, never NULL */
		return PTR_ERR(worker);		/* convert the error pointer to -errno; load fails */
	return 0;				/* success: thread is running, module is Live */
}						/* end of kthread_demo_init() */
static void __exit kthread_demo_exit(void)	/* runs at rmmod */
{						/* start of kthread_demo_exit() */
	kthread_stop(worker);			/* set should_stop, wake the thread, wait for it to return */
}						/* end: thread gone, safe to unload the code it ran */
module_init(kthread_demo_init);			/* register the entry point */
module_exit(kthread_demo_exit);			/* register the exit point */
MODULE_LICENSE("GPL");				/* GPL-compatible licence: needed for GPL-only kthread symbols */
MODULE_DESCRIPTION("Minimal kernel thread demo");	/* shown by modinfo */
