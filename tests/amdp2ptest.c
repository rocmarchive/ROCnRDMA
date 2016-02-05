/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/uaccess.h>

#include "amd_rdma.h"
#include "amdp2ptest.h"


MODULE_AUTHOR("serguei.sagalovitch@amd.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD RDMA basic API test kernel-mode driver");
MODULE_VERSION("1.0");


const struct amd_rdma_interface *rdma_interface;


struct amdp2ptest_node {
	struct list_head list_node;
	struct amd_p2p_info *info;
	struct mutex *lock;
};


struct amdp2ptest_list {
	struct list_head	head;
	struct mutex	lock;
};


#define MSG_INFO(fmt, args ...)	\
			pr_info(AMDP2PTEST_DEVICE_NAME ": " fmt, ## args)
#define MSG_ERR(fmt, args ...)	\
			pr_err(AMDP2PTEST_DEVICE_NAME ": " fmt, ## args)
#define MSG_warn(fmt, args ...)	\
			pr_warn(AMDP2PTEST_DEVICE_NAME ": " fmt, ## args)



static void free_callback(void *client_priv)
{
	struct amdp2ptest_node *node = client_priv;

	MSG_ERR("Free callback is called on va 0x%llx, size 0x%llx\n",
			node->info->va, node->info->size);

	mutex_lock(node->lock);
	list_del(&node->list_node);
	mutex_unlock(node->lock);

	kfree(node);
}



static int amdp2ptest_open(struct inode *inode, struct file *filp)
{
	struct amdp2ptest_list *list;

	MSG_INFO("Open driver\n");

	list = kmalloc(sizeof(struct amdp2ptest_list), GFP_KERNEL);

	if (!list) {
		MSG_ERR("Can't alloc kernel memory to store list stucture\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&list->head);
	mutex_init(&list->lock);

	filp->private_data = list;

	return 0;
}


static int amdp2ptest_release(struct inode *inode, struct file *filp)
{
	struct amdp2ptest_node *tmp, *node = NULL;
	int retcode;
	struct amdp2ptest_list *list = filp->private_data;

	MSG_INFO("Close driver\n");

	list_for_each_entry_safe(node, tmp, &list->head, list_node) {
		MSG_INFO("Free pages: VA 0x%llx\n", node->info->va);
		retcode = rdma_interface->put_pages(&node->info);

		if (retcode != 0)
			MSG_ERR("Could not put pages back: %d\n", retcode);

		mutex_lock(&list->lock);
		list_del(&node->list_node);
		mutex_unlock(&list->lock);
		kfree(node);
	}

	filp->private_data = NULL;
	kfree(list);
	return 0;
}

static int ioctl_is_gpu_address(struct file *filp, unsigned long arg)
{
	struct AMDRDMA_IOCTL_IS_GPU_ADDRESS_PARAM params = {0};

	MSG_INFO("AMD2P2PTEST_IOCTL_IS_GPU_ADDRESS");

	if (copy_from_user(&params, (void *)arg, sizeof(params))) {
		MSG_ERR("copy_from_user failed on pointer %p\n",
							(void *)arg);
		return -EFAULT;
	}

	params.ret_value = rdma_interface->is_gpu_address(params.addr,
				get_task_pid(current, PIDTYPE_PID));

	if (copy_to_user((void *)arg, &params, sizeof(params))) {
		MSG_ERR("copy_to_user failed on user pointer %p\n",
						(void *)arg);

		return -EFAULT;
	}

	return 0;

}


static int ioctl_get_page_size(struct file *filp, unsigned long arg)
{
	struct AMDRDMA_IOCTL_GET_PAGE_SIZE_PARAM params = {0};
	unsigned long page_size;
	int result;

	MSG_INFO("AMD2P2PTEST_IOCTL_GET_PAGE_SIZE");

	if (copy_from_user(&params, (void *)arg, sizeof(params))) {
		MSG_ERR("copy_from_user failed on pointer %p\n",
							(void *)arg);
		return -EFAULT;
	}

	MSG_INFO("addr %llx, length %llx\n", params.addr,
					     params.length);
	result = rdma_interface->get_page_size(params.addr,
				params.length,
				get_task_pid(current, PIDTYPE_PID),
				&page_size);

	if (result) {
		MSG_ERR("Could not get page size. %d", result);
		return -EFAULT;
	}

	params.page_size = page_size;
	MSG_INFO("Page size %llx\n", params.page_size);

	if (copy_to_user((void *)arg, &params, sizeof(params))) {
		MSG_ERR("copy_to_user failed on user pointer %p\n",
						(void *)arg);

		return -EFAULT;
	}

	return 0;
}

static int ioctl_get_pages(struct file *filp, unsigned long arg)
{
	struct amdp2ptest_node *node = NULL;
	struct amdp2ptest_list *list = filp->private_data;
	struct AMDRDMA_IOCTL_GET_PAGES_PARAM params = {0};
	int result;


	MSG_INFO("AMD2P2PTEST_IOCTL_GET_PAGES");

	if (copy_from_user(&params, (void *)arg, sizeof(params))) {
		MSG_ERR("copy_from_user failed on pointer %p\n",
							(void *)arg);
		return -EFAULT;
	}

	MSG_INFO("addr %llx, length %llx\n", params.addr, params.length);

	node = kmalloc(sizeof(struct amdp2ptest_node), GFP_KERNEL);

	if (node == 0) {
		MSG_ERR("Can't alloc kernel memory\n");
		return -ENOMEM;
	}

	memset(node, 0, sizeof(struct amdp2ptest_node));

	node->lock = &list->lock;

	result = rdma_interface->get_pages(params.addr, params.length,
					get_task_pid(current, PIDTYPE_PID),
					&node->info,
					free_callback,
					node /* Pointer to the list */
					);

	if (result) {
		MSG_ERR("Could not get pages table. %d", result);
		return -EFAULT;
	}

	if (copy_to_user((void *)arg, &params, sizeof(params))) {
		MSG_ERR("copy_to_user failed on user pointer %p\n",
							(void *)arg);
		rdma_interface->put_pages(&node->info);
		return -EFAULT;
	}

	mutex_lock(&list->lock);
	list_add(&node->list_node, &list->head);
	mutex_unlock(&list->lock);

	return 0;
}


static int ioctl_put_pages(struct file *filp, unsigned long arg)
{
	struct amdp2ptest_node *tmp, *node;
	struct amdp2ptest_list *list = filp->private_data;
	struct AMDRDMA_IOCTL_PUT_PAGES_PARAM params = {0};
	int retcode;

	MSG_INFO("AMD2P2PTEST_IOCTL_PUT_PAGES");

	if (copy_from_user(&params, (void *)arg, sizeof(params))) {
		MSG_ERR("copy_from_user failed on pointer %p\n",
							(void *)arg);
		return -EFAULT;
	}

	MSG_INFO("addr %llx, length %llx\n", params.addr, params.length);


	list_for_each_entry_safe(node, tmp, &list->head, list_node) {
		if (node->info->va == params.addr &&
			node->info->size == params.length) {

			retcode = rdma_interface->put_pages(&node->info);

			if (retcode != 0) {
				MSG_ERR("Could not put pages back: %d\n",
						retcode);
			}

			mutex_lock(&list->lock);
			list_del(&node->list_node);
			mutex_unlock(&list->lock);
			kfree(node);
			/* Note: Do not break from loop to allow test
			 * situation when "get_pages" would be called
			 * on the same memory several times
			 **/
		}
	}

	return 0;
}


static const struct ioctl_handler_map {
	int (*handler)(struct file *filp, unsigned long arg);
	unsigned int cmd;
} handlers[] = {
	{ ioctl_get_page_size,	AMD2P2PTEST_IOCTL_GET_PAGE_SIZE },
	{ ioctl_get_pages,	AMD2P2PTEST_IOCTL_GET_PAGES	},
	{ ioctl_put_pages,	AMD2P2PTEST_IOCTL_PUT_PAGES	},
	{ ioctl_is_gpu_address,	AMD2P2PTEST_IOCTL_IS_GPU_ADDRESS },
	{ NULL, 0 }
};



static long amdp2ptest_unlocked_ioctl(struct file *filp, unsigned int cmd,
							 unsigned long arg)
{
	int result = -EINVAL;
	int i;

	for (i = 0; handlers[i].handler != NULL; i++)
		if (cmd == handlers[i].cmd) {
			result = handlers[i].handler(filp, arg);
			break;
		}

	return result;
}


static int amdp2ptest_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int i;
	struct scatterlist *sg;
	struct amdp2ptest_node *tmp, *node;
	struct amdp2ptest_list *list = filp->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	uint64_t gpu_va = vma->vm_pgoff << PAGE_SHIFT;
	uint64_t start_addr;

	MSG_INFO("Mapping to CPU user space\n");
	MSG_INFO("Begin vm_start 0x%lx, vm_end 0x%lx\n", vma->vm_start,
			vma->vm_end);
	MSG_INFO("vm_pgoff / pfn 0x%lx\n", vma->vm_pgoff);
	MSG_INFO("gpu_va / phys. address 0x%llx\n", gpu_va);

	/*
	* The logic is the following:
	*	- We get GPU VA address and enumerate list to find "get_pages"
	*	  node  for such range
	*	- Then we enumerate sg table to find the correct dma_address
	*
	* NOTE/TODO: Assumption is that the page size is 4KB to allow testing
	* of the basic logic. Eventually more complex logic must be added.
	*/
	list_for_each_entry_safe(node, tmp, &list->head, list_node) {
		if (node->info->va >= gpu_va  &&
		    (node->info->va + node->info->size) <= gpu_va + size) {

			MSG_INFO("Found node: va=0x%llx,size=0x%llx,nents %d\n",
					node->info->va,
					node->info->size,
					node->info->pages->nents);

			start_addr = vma->vm_start;

			for_each_sg(node->info->pages->sgl, sg,
					node->info->pages->nents, i) {

					MSG_INFO("Found page[%d]: dma 0x%llx\n",
							i, sg->dma_address);

					if (remap_pfn_range(vma,
							start_addr,
							sg->dma_address >> PAGE_SHIFT,
							size,
							vma->vm_page_prot)) {
						MSG_ERR("Failed remap_pfn()\n");
						return -EINVAL;
					}

					start_addr += sg->length;

					return 0;
			}
		}
	}

	return -EINVAL;
}


/*---------------------------------------------------------------------------*/

static const struct file_operations amdp2ptest_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = amdp2ptest_unlocked_ioctl,
	.open = amdp2ptest_open,
	.release = amdp2ptest_release,
	.mmap = amdp2ptest_mmap,
};



static struct miscdevice amdp2ptest_dev = {
	/*
	 * We don't care what minor number we end up with, so tell the
	 * kernel to just pick one.
	 */
	.minor = MISC_DYNAMIC_MINOR,
	/*
	 * Name ourselves /dev/hello.
	 */
	.name = AMDP2PTEST_DEVICE_NAME,
	/*
	 * What functions to call when a program performs file
	 * operations on the device.
	 */
	.fops = &amdp2ptest_fops,

	/* Security attribute / access */
	.mode = S_IRWXU | S_IRWXG | S_IRWXO
};

static int __init amdp2ptest_init(void)
{
	int result;

	result = amdkfd_query_rdma_interface(&rdma_interface);

	if (result < 0) {
		MSG_ERR("Can not get RDMA Interface (result = %d)\n", result);
		return result;
	}

	MSG_INFO("RDMA Interface %p\n",		rdma_interface);
	MSG_INFO("     get_pages %p\n",		rdma_interface->get_pages);
	MSG_INFO("     put_pages %p\n",		rdma_interface->put_pages);
	MSG_INFO("     is_gpu_address %p\n",	rdma_interface->is_gpu_address);
	MSG_INFO("     get_page_size %p\n",	rdma_interface->get_page_size);


	/*
	* Create the device in the /sys/class/misc directory.
	* Udev will automatically create the /dev/xxxxx device using
	* the default rules.
	*/
	result  = misc_register(&amdp2ptest_dev);

	if (result < 0) {
		MSG_ERR("Can not register device (result = %d)\n", result);
		return result;
	}

	return 0;
}


/* Note: cleanup_module is never called if registering failed */
static void __exit amdp2ptest_cleanup(void)
{
	MSG_INFO("Unregistering\n");

	misc_deregister(&amdp2ptest_dev);
}


module_init(amdp2ptest_init);
module_exit(amdp2ptest_cleanup);


