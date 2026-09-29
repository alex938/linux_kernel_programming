// SPDX-License-Identifier: GPL-2.0
/* ^ SPDX tag: machine-readable licence of this file (required in kernel sources) */
/*
 * kprobe_demo.c - log every call to kernel_clone() (fork/clone) using a kprobe.
 */
#include <linux/init.h>		/* __init / __exit section markers */
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() macros */
#include <linux/kprobes.h>	/* struct kprobe, register_kprobe(), unregister_kprobe() */
#include <linux/sched.h>	/* current, task_pid_nr() */
#include <linux/printk.h>	/* pr_info(), pr_info_ratelimited(), pr_err() */
static char symbol[KSYM_NAME_LEN] = "kernel_clone";	/* function to probe; default is the fork/clone path */
module_param_string(symbol, symbol, sizeof(symbol), 0444);	/* allow symbol=<name> at insmod, read-only in sysfs */
MODULE_PARM_DESC(symbol, "Kernel function to probe (default kernel_clone)");	/* description shown by modinfo */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)	/* called just before the probed instruction runs */
{									/* start of handler_pre(): atomic context, must not sleep */
	pr_info_ratelimited("kprobe_demo: %s hit by %s (pid %d)\n",	/* rate-limited so a busy probe cannot flood the log */
			    p->symbol_name, current->comm,		/* probed symbol name and the calling task's name */
			    task_pid_nr(current));			/* calling task's PID */
	return 0;							/* 0 = continue and execute the probed instruction normally */
}									/* end of handler_pre() */
static struct kprobe kp = {		/* the probe descriptor, registered in init */
	.pre_handler = handler_pre,	/* run handler_pre() on every hit */
};					/* .symbol_name is filled in at init time from the module parameter */
static int __init kprobe_demo_init(void)	/* runs once at insmod */
{						/* start of kprobe_demo_init() */
	int ret;				/* return code from register_kprobe() */
	kp.symbol_name = symbol;		/* probe by symbol name; the kernel resolves the address via kallsyms */
	ret = register_kprobe(&kp);		/* insert the breakpoint (int3 on x86) at the symbol's address */
	if (ret < 0) {				/* e.g. -EINVAL for a blacklisted/inlined symbol, -ENOENT if not found */
		pr_err("kprobe_demo: register_kprobe(%s) failed: %d\n", symbol, ret);	/* explain why the load failed */
		return ret;			/* non-zero return = module load fails; nothing to unwind yet */
	}					/* end of error branch */
	pr_info("kprobe_demo: planted at %s (%pS)\n", symbol, kp.addr);	/* %pS prints symbol+offset (address hidden by kptr_restrict) */
	return 0;				/* 0 = success, module becomes "Live" */
}					/* end of kprobe_demo_init() */
static void __exit kprobe_demo_exit(void)	/* runs at rmmod */
{						/* start of kprobe_demo_exit() */
	unregister_kprobe(&kp);			/* remove the breakpoint; must happen before our handler code is freed */
	pr_info("kprobe_demo: removed from %s\n", symbol);	/* log the removal */
}						/* end of kprobe_demo_exit() */
module_init(kprobe_demo_init);			/* register kprobe_demo_init() as the module's entry point */
module_exit(kprobe_demo_exit);			/* register kprobe_demo_exit() as the module's exit point */
MODULE_LICENSE("GPL");				/* required: register_kprobe() is EXPORT_SYMBOL_GPL */
MODULE_DESCRIPTION("kprobe demo: log calls to a kernel function");	/* shown by modinfo */
MODULE_AUTHOR("Advanced Linux Kernel Programming course");	/* shown by modinfo */
