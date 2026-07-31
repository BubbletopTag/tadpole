/* drivers/lf1000/ga3d.c
 *
 * Copyright 2007-2010 LeapFrog Enterprises Inc.
 *
 * LF1000 3D accelerator driver 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <mach/platform.h>

#define GA3D_MAJOR 	249	

struct ga3d_device {
	void __iomem *mem;
	struct cdev *cdev;
	dev_t dev;
	int major;
	struct proc_dir_entry *proc;
};

/* device private data */
static struct ga3d_device ga3d = {
	.mem = NULL,
	.cdev = NULL,
	.major = GA3D_MAJOR,
};

/*******************************
 * character device operations *
 *******************************/

static int ga3d_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static void ga3d_vma_open(struct vm_area_struct *vma)
{
	/* printk(KERN_DEBUG "ga3d: vma_open virt:%lX, phs %lX\n",
	       vma->vm_start, vma->vm_pgoff<<PAGE_SHIFT); */
}

static void ga3d_vma_close(struct vm_area_struct *vma)
{
	/* printk(KERN_DEBUG "ga3d: vma_close\n"); */
}

static struct vm_operations_struct ga3d_vm_ops = {
	.open  = ga3d_vma_open,
	.close = ga3d_vma_close,
};

static int ga3d_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_ops = &ga3d_vm_ops;
	vma->vm_flags |= VM_IO;
	
	ret = io_remap_pfn_range(vma,
				 vma->vm_start, 
				 0xc001a000>>PAGE_SHIFT,
				 vma->vm_end - vma->vm_start, 
				 vma->vm_page_prot);
	if(ret < 0) {
		printk(KERN_ALERT "ga3d: failed to mmap\n");
		return -EAGAIN;
	}
	
	ga3d_vma_open(vma);
	return 0;
}

static struct file_operations ga3d_fops = {
	.owner = THIS_MODULE,
	.open  = ga3d_open,
	.mmap = ga3d_mmap,
};

/*********************
 *  module functions *
 *********************/

static int lf1000_ga3d_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(!res) {
		dev_err(&pdev->dev, "failed to get resource\n");
		return -ENXIO;
	}

	if(!request_mem_region(res->start, (res->end - res->start)+1,
				"lf1000-ga3d")) {
		dev_err(&pdev->dev, "failed to map region.");
		return -EBUSY;
	}

	ga3d.mem = ioremap_nocache(res->start, (res->end - res->start)+1);
	if(ga3d.mem == NULL) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		ret = -ENOMEM;
		goto fail_remap;
	}

	ret = register_chrdev(ga3d.major, "ga3d", &ga3d_fops);
	if(ret < 0) {
		dev_err(&pdev->dev, "failed to get a device\n");
		goto fail_dev;
	}
	if(ga3d.major == 0) ga3d.major = ret;

	ga3d.cdev = cdev_alloc();
	ga3d.cdev->owner = THIS_MODULE;
	ga3d.cdev->ops = &ga3d_fops;
	ret = cdev_add(ga3d.cdev, 0, 1);
	if(ret < 0) {
		dev_err(&pdev->dev, "failed to create character device\n");
		goto fail_add;
	}

	return 0;

fail_add:
	unregister_chrdev(ga3d.major, "ga3d");
fail_dev:
fail_remap:
	release_mem_region(res->start, (res->end - res->start) + 1);

	return ret;
}

static int lf1000_ga3d_remove(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	unregister_chrdev(ga3d.major, "ga3d");
	if(ga3d.cdev != NULL)
		cdev_del(ga3d.cdev);
	
	if(ga3d.mem != NULL)
		iounmap(ga3d.mem);

	release_mem_region(res->start, (res->end - res->start) + 1);

	return 0;
}

static struct platform_driver lf1000_ga3d_driver = {
	.probe		= lf1000_ga3d_probe,
	.remove		= lf1000_ga3d_remove,
	.driver		= {
		.name	= "lf1000-ga3d",
		.owner	= THIS_MODULE,
	},
};

static int __init ga3d_init(void)
{
	return platform_driver_register(&lf1000_ga3d_driver);
}

static void __exit ga3d_cleanup(void)
{
	platform_driver_unregister(&lf1000_ga3d_driver);
}

module_init(ga3d_init);
module_exit(ga3d_cleanup);
MODULE_AUTHOR("Brian Cavagnolo, Andrey Yurovsky");
MODULE_LICENSE("GPL");
