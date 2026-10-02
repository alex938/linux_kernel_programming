// SPDX-License-Identifier: GPL-2.0
/*
 * kmalloc_demo.c - the main kernel allocators side by side:
 * kmalloc/kzalloc (slab, physically contiguous), vmalloc (virtually
 * contiguous only), a dedicated kmem_cache, and alloc_pages (buddy).
 */
#include <linux/module.h>			/* module_init(), MODULE_*() */
#include <linux/slab.h>				/* kmalloc(), kzalloc(), kfree(), kmem_cache_*() */
#include <linux/vmalloc.h>			/* vmalloc(), vfree(), is_vmalloc_addr() */
#include <linux/gfp.h>				/* alloc_pages(), __free_pages(), GFP_* flags */
#include <linux/mm.h>				/* page_address(), PAGE_SIZE */
#include <linux/io.h>				/* virt_to_phys() */

struct demo_obj {				/* a small fixed-size object for the slab cache */
	int id;					/* example field */
	char name[28];				/* example field: makes the object 32 bytes */
};

static void *kbuf;				/* kmalloc'd buffer: physically contiguous */
static void *vbuf;				/* vmalloc'd buffer: virtually contiguous only */
static struct kmem_cache *demo_cache;		/* dedicated slab cache for struct demo_obj */
static struct demo_obj *obj;			/* one object taken from demo_cache */
static struct page *pages;			/* block of 2^order pages from the buddy allocator */
static const unsigned int order = 2;		/* 2^2 = 4 contiguous pages */

static int __init kmalloc_demo_init(void)	/* runs at insmod; __init: freed after it returns */
{
	int ret = -ENOMEM;			/* every failure below is out of memory */

	kbuf = kzalloc(1024, GFP_KERNEL);	/* 1 KiB, zeroed; GFP_KERNEL may sleep (process context) */
	if (!kbuf)				/* kmalloc family returns NULL on failure */
		goto out;			/* nothing to undo yet */

	vbuf = vmalloc(4 * 1024 * 1024);	/* 4 MiB: too big to want physically contiguous */
	if (!vbuf)				/* vmalloc also returns NULL on failure */
		goto free_kbuf;			/* undo the kzalloc */

	demo_cache = KMEM_CACHE(demo_obj, 0);	/* slab cache sized/aligned for struct demo_obj */
	if (!demo_cache)			/* cache creation can fail */
		goto free_vbuf;			/* undo vmalloc and kzalloc */

	obj = kmem_cache_zalloc(demo_cache, GFP_KERNEL);	/* take one zeroed object from the cache */
	if (!obj)				/* allocation from the cache failed */
		goto destroy_cache;		/* undo the cache and earlier allocations */

	pages = alloc_pages(GFP_KERNEL, order);	/* 4 physically contiguous pages from the buddy allocator */
	if (!pages)				/* higher orders fail more easily (fragmentation) */
		goto free_obj;			/* undo everything allocated so far */

	pr_info("kmalloc: virt=%px phys=%pa\n",	/* show a direct-map address and its physical address */
		kbuf, &(phys_addr_t){ virt_to_phys(kbuf) });	/* virt_to_phys() is valid for kmalloc memory */
	pr_info("vmalloc: virt=%px is_vmalloc=%d\n",	/* vmalloc memory lives in the vmalloc area */
		vbuf, is_vmalloc_addr(vbuf));	/* virt_to_phys() would be WRONG here */
	pr_info("pages: order=%u virt=%px\n",	/* buddy pages are reachable via the direct map */
		order, page_address(pages));	/* page_address(): struct page -> kernel virtual address */
	return 0;				/* success: module stays loaded */

free_obj:					/* error unwinding, in reverse order of allocation */
	kmem_cache_free(demo_cache, obj);	/* return the object to its cache */
destroy_cache:
	kmem_cache_destroy(demo_cache);		/* destroy the (now empty) cache */
free_vbuf:
	vfree(vbuf);				/* release the vmalloc area */
free_kbuf:
	kfree(kbuf);				/* release the kmalloc buffer */
out:
	return ret;				/* insmod reports "Cannot allocate memory" */
}

static void __exit kmalloc_demo_exit(void)	/* runs at rmmod */
{
	__free_pages(pages, order);		/* order must match the alloc_pages() call */
	kmem_cache_free(demo_cache, obj);	/* free the object before destroying its cache */
	kmem_cache_destroy(demo_cache);		/* the cache must be empty here */
	vfree(vbuf);				/* free the vmalloc buffer */
	kfree(kbuf);				/* free the kmalloc buffer */
	pr_info("kmalloc_demo: freed everything\n");	/* confirm in dmesg */
}

module_init(kmalloc_demo_init);			/* register the init function */
module_exit(kmalloc_demo_exit);			/* register the exit function */

MODULE_LICENSE("GPL");				/* GPL: required for GPL-only exports, avoids taint */
MODULE_DESCRIPTION("kmalloc, vmalloc, kmem_cache and alloc_pages side by side");	/* shown by modinfo */
