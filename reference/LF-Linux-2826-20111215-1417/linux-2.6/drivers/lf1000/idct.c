/* LF1000 IDCT Macro Block Decoder driver 
 *
 * main.c -- Main driver functionality.
 *
 * Brian Cavagnolo <brian@cozybit.com>
 * Andrey Yurovsky <andrey@cozybit.com>
 * Dave Milici <dmilici@leapfrog.com>
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <mach/platform.h>

/* Register offsets */
#define IDCT_BUF_DATA           (0x00)
#define IDCT_CONTROL            (0x80)
#define IDCT_INT_ENB            (0x84)
#define IDCT_INT_PEND           (0x88)
#define IDCT_CLK_ENB            (0x7C0)

#define IDCT_MAJOR      248

struct idct_device {
	void __iomem *mem;
	struct cdev *cdev;
	dev_t dev;
	int major;
	struct dentry *debug;
};

static struct idct_device idct = {
	.mem = NULL,
	.cdev = NULL,
	.major = IDCT_MAJOR,
};

static void idct_reg(struct seq_file *s, const char *nm, u32 reg)
{
	struct idct_device *dev = s->private;

	seq_printf(s, "%10s:\t0x%08X\n", nm, readl(dev->mem + reg));
}

static int idct_show_registers(struct seq_file *s, void *v)
{
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x04);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x08);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x0C);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x10);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x14);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x18);
	idct_reg(s, "BUF_DATA", IDCT_BUF_DATA + 0x1C);
	idct_reg(s, "CONTROL",	IDCT_CONTROL);
	idct_reg(s, "INT_ENB",	IDCT_INT_ENB);
	idct_reg(s, "INT_PEND", IDCT_INT_PEND);
	idct_reg(s, "CLK_ENB",	IDCT_CLK_ENB);

	return 0;
}

static int lf1000_idct_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, idct_show_registers, inode->i_private);
}

static const struct file_operations lf1000_idct_regs_fops = {
	.owner          = THIS_MODULE,
	.open           = lf1000_idct_regs_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

/*******************************
 * character device operations *
 *******************************/

static int idct_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static void idct_vma_open(struct vm_area_struct *vma)
{
}

static void idct_vma_close(struct vm_area_struct *vma)
{
}

static struct vm_operations_struct idct_vm_ops = {
	.open  = idct_vma_open,
	.close = idct_vma_close,
};

static int idct_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_ops = &idct_vm_ops;
	vma->vm_flags |= VM_IO;
	
	ret = io_remap_pfn_range(vma,
				 vma->vm_start, 
				 0xC000F800>>PAGE_SHIFT,
				 vma->vm_end - vma->vm_start, 
				 vma->vm_page_prot);
	if(ret < 0)
		return -EAGAIN;
	
	idct_vma_open(vma);
	return 0;
}

static struct file_operations idct_fops = {
	.owner = THIS_MODULE,
	.open  = idct_open,
	.mmap = idct_mmap,
};

/*********************
 *  module functions *
 *********************/

static int lf1000_idct_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if(!res) {
		dev_err(&pdev->dev, "failed to get resource\n");
		return -ENXIO;
	}

	if(!request_mem_region(res->start, (res->end - res->start)+1,
				"lf1000-idct")) {
		dev_err(&pdev->dev, "failed to map region.");
		return -EBUSY;
	}

	idct.mem = ioremap_nocache(res->start, (res->end - res->start)+1);
	if(idct.mem == NULL) {
		dev_err(&pdev->dev, "failed to ioremap\n");
		ret = -ENOMEM;
		goto fail_remap;
	}

	ret = register_chrdev(idct.major, "idct", &idct_fops);
	if(ret < 0) {
		dev_err(&pdev->dev, "failed to get a device\n");
		goto fail_dev;
	}
	if(idct.major == 0) idct.major = ret;

	idct.cdev = cdev_alloc();
	idct.cdev->owner = THIS_MODULE;
	idct.cdev->ops = &idct_fops;
	ret = cdev_add(idct.cdev, 0, 1);
	if(ret < 0) {
		dev_err(&pdev->dev, "failed to create character device\n");
		goto fail_add;
	}

	idct.debug = debugfs_create_dir("lf1000-idct", NULL);
	if (!idct.debug || IS_ERR(idct.debug))
		idct.debug = NULL;
	else
		debugfs_create_file("registers", S_IRUSR, idct.debug, &idct,
				&lf1000_idct_regs_fops);

	return 0;

fail_add:
	unregister_chrdev(idct.major, "idct");
fail_dev:
fail_remap:
	release_mem_region(res->start, (res->end - res->start) + 1);

	return ret;
}

static int lf1000_idct_remove(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (idct.debug) {
		debugfs_remove_recursive(idct.debug);
		idct.debug = NULL;
	}

	unregister_chrdev(idct.major, "idct");
	if(idct.cdev != NULL)
		cdev_del(idct.cdev);
	
	if(idct.mem != NULL)
		iounmap(idct.mem);

	release_mem_region(res->start, (res->end - res->start) + 1);

	return 0;
}

static struct platform_driver lf1000_idct_driver = {
	.probe		= lf1000_idct_probe,
	.remove		= lf1000_idct_remove,
	.driver		= {
		.name	= "lf1000-idct",
		.owner	= THIS_MODULE,
	},
};

static int __init idct_init(void)
{
	return platform_driver_register(&lf1000_idct_driver);
}

static void __exit idct_cleanup(void)
{
	platform_driver_unregister(&lf1000_idct_driver);
}

module_init(idct_init);
module_exit(idct_cleanup);
MODULE_LICENSE("GPL");
