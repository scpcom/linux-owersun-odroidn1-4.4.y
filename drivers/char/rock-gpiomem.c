/*
 * linux/drivers/char/rock-gpiomem.c
 *
 * GPIO memory device driver
 *
 * Creates a chardev /dev/gpiomem which will provide user access to
 * the Rockchip's GPIO registers when it is mmap()'d.
 * No longer need root for user GPIO access, but without relaxing permissions
 * on /dev/mem.
 *
 * Copyright (c) 2017 Hardkernel Co., Ltd.
 *
 * This driver is based on bcm2835-gpiomem.c in Raspberrypi's linux kernel 4.4:
 *	Written by Luke Wren <luke@raspberrypi.org>
 *	Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/pagemap.h>
#include <linux/io.h>
#include <linux/of.h>
#include <asm/io.h>

#define DEVICE_NAME "rock-gpiomem"
#define DRIVER_NAME "gpiomem-rock"
#define DEVICE_MINOR 0

struct regs_phys {
	unsigned long start;
	unsigned long end;
};

struct rock_gpiomem_instance {
	struct regs_phys gpio_regs_phys[32];
	int gpio_area_count;
	struct device *dev;
};

static struct cdev rock_gpiomem_cdev;
static dev_t rock_gpiomem_devid;
static struct class *rock_gpiomem_class;
static struct device *rock_gpiomem_dev;
static struct rock_gpiomem_instance *inst;

static int rock_gpiomem_open(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);
	int ret = 0;

	dev_info(inst->dev, "gpiomem device opened.");

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev, "Unknown minor device: %d", dev);
		ret = -ENXIO;
	}
	return ret;
}

static int rock_gpiomem_release(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);
	int ret = 0;

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev, "Unknown minor device %d", dev);
		ret = -ENXIO;
	}
	return ret;
}

static const struct vm_operations_struct rock_gpiomem_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

static int rock_gpiomem_mmap(struct file *file, struct vm_area_struct *vma)
{
	int gpio_area = 0;
	unsigned long start = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long end   = start + vma->vm_end - vma->vm_start;

	while (gpio_area < inst->gpio_area_count) {
		if ((inst->gpio_regs_phys[gpio_area].start <= start) &&
		    (inst->gpio_regs_phys[gpio_area].end   >= end))
			goto found;
		gpio_area++;
	}

	return -EACCES;

found:
	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff,
			vma->vm_end - vma->vm_start,
			vma->vm_page_prot);

	vma->vm_ops = &rock_gpiomem_vm_ops;

	if (remap_pfn_range(vma, vma->vm_start,
				vma->vm_pgoff,
				vma->vm_end - vma->vm_start,
				vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}

static const struct file_operations
rock_gpiomem_fops = {
	.owner = THIS_MODULE,
	.open = rock_gpiomem_open,
	.release = rock_gpiomem_release,
	.mmap = rock_gpiomem_mmap,
};

static int rock_gpiomem_probe(struct platform_device *pdev)
{
	int err = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res = NULL;
	int i = 0;

	/* Allocate buffers and instance data */
	inst = kzalloc(sizeof(struct rock_gpiomem_instance), GFP_KERNEL);

	if (!inst) {
		err = -ENOMEM;
		goto failed_inst_alloc;
	}

	inst->dev = dev;
	inst->gpio_area_count = of_property_count_elems_of_size(np, "reg",
				sizeof(u32)) / 4;

	if (inst->gpio_area_count > 32 || inst->gpio_area_count <= 0) {
		dev_err(inst->dev, "failed to get gpio register area.");
		err = -EINVAL;
		goto failed_inst_alloc;
	}

	dev_info(inst->dev, "Initialised: GPIO register area is %d",
			inst->gpio_area_count);

	for (i = 0; i < inst->gpio_area_count; ++i) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (res) {
			inst->gpio_regs_phys[i].start = res->start;
			inst->gpio_regs_phys[i].end   = res->end;
		} else {
			dev_err(inst->dev, "failed to get IO resource area %d", i);
			err = -ENOENT;
			goto failed_get_resource;
		}
	}

	/* Create character device entries */
	err = alloc_chrdev_region(&rock_gpiomem_devid,
				  DEVICE_MINOR, 1, DEVICE_NAME);
	if (err != 0) {
		dev_err(inst->dev, "unable to allocate device number");
		goto failed_alloc_chrdev;
	}
	cdev_init(&rock_gpiomem_cdev, &rock_gpiomem_fops);
	rock_gpiomem_cdev.owner = THIS_MODULE;
	err = cdev_add(&rock_gpiomem_cdev, rock_gpiomem_devid, 1);
	if (err != 0) {
		dev_err(inst->dev, "unable to register device");
		goto failed_cdev_add;
	}

	/* Create sysfs entries */
	rock_gpiomem_class = class_create(THIS_MODULE, DEVICE_NAME);
	err = IS_ERR(rock_gpiomem_class);
	if (err)
		goto failed_class_create;

	rock_gpiomem_dev = device_create(rock_gpiomem_class, NULL,
					rock_gpiomem_devid, NULL,
					"gpiomem");
	err = IS_ERR(rock_gpiomem_dev);
	if (err)
		goto failed_device_create;

	for (i = 0; i < inst->gpio_area_count; ++i) {
		dev_info(inst->dev,
			"Initialised: Registers at start:0x%08lx end:0x%08lx size:0x%08lx",
			inst->gpio_regs_phys[i].start,
			inst->gpio_regs_phys[i].end,
			inst->gpio_regs_phys[i].end - inst->gpio_regs_phys[i].start);
	}

	return 0;

failed_device_create:
	class_destroy(rock_gpiomem_class);
failed_class_create:
	cdev_del(&rock_gpiomem_cdev);
failed_cdev_add:
	unregister_chrdev_region(rock_gpiomem_devid, 1);
failed_alloc_chrdev:
failed_get_resource:
	kfree(inst);
failed_inst_alloc:
	dev_err(inst->dev, "could not load rock_gpiomem");
	return err;
}

static int rock_gpiomem_remove(struct platform_device *pdev)
{
	struct device *dev = inst->dev;

	kfree(inst);
	device_destroy(rock_gpiomem_class, rock_gpiomem_devid);
	class_destroy(rock_gpiomem_class);
	cdev_del(&rock_gpiomem_cdev);
	unregister_chrdev_region(rock_gpiomem_devid, 1);

	dev_info(dev, "GPIO mem driver removed - OK");
	return 0;
}

static const struct of_device_id rock_gpiomem_of_match[] = {
	{.compatible = "rockchip,rock-gpiomem",},
	{ },
};
MODULE_DEVICE_TABLE(of, rock_gpiomem_of_match);

static struct platform_driver rock_gpiomem_driver = {
	.driver			= {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= rock_gpiomem_of_match,
	},
	.probe			= rock_gpiomem_probe,
	.remove			= rock_gpiomem_remove,
};

module_platform_driver(rock_gpiomem_driver);

MODULE_ALIAS("platform:gpiomem-rock");
MODULE_DESCRIPTION("ROCKCHIP gpiomem driver for accessing GPIO from userspace");
MODULE_AUTHOR("Brian Kim <brian.kim@hardkernel.com>");
MODULE_LICENSE("GPL");
