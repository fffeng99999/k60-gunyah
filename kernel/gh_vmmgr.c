// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah VM manager for Redmi K60 (mondrian, SM8475).
 *
 * This module is additive: it does NOT replace the vendor gh_rm_drv /
 * gh_arm_drv / gh_msgq / gh_dbl drivers, which DRM, qrtr and virtio
 * backends already depend on. It layers on top of the exported
 * gh_rm_* client API and creates the /dev/gunyah character device that
 * crosvm expects.
 *
 * Design constraints:
 *   - No kprobe, no private symbol lookup, no address hardcoding.
 *   - Every RM call goes through the exported gh_rm_* API so the module
 *     stays valid across vendor RM updates.
 *   - The device may reject non-trusted VM allocation. When that happens
 *     the failure is reported with the RM error code and the module
 *     refuses to create the VM rather than fall back to an undocumented
 *     path.
 *   - Autoload is off by default. The Magisk module must explicitly
 *     enable it.
 */

#define pr_fmt(fmt) "gh_vmmgr: " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/moduleparam.h>

#include <linux/gunyah/gh_common.h>
#include <linux/gunyah/gh_rm_drv.h>

#include "gh_vmmgr.h"

static dev_t gh_vmmgr_dev;
static struct cdev gh_vmmgr_cdev;
static struct class *gh_vmmgr_class;

static DEFINE_MUTEX(gh_vmmgr_lock);
static unsigned int gh_vmmgr_vmid;
static bool gh_vmmgr_registered;

/*
 * probe_rm=1 makes the module perform the RM queries at insmod time and
 * print the result, so behaviour can be observed without a userspace
 * helper. Default 0: load only creates the device node.
 */
static bool probe_rm;
module_param(probe_rm, bool, 0400);
MODULE_PARM_DESC(probe_rm,
	"Run read-only RM queries at insmod and print result (default N)");

/*
 * The vendor gh_vm_names enum is fixed and has no entry for a user
 * supplied VM, so gh_rm_vm_alloc_vmid() cannot allocate arbitrary VMs.
 * We still drive the exported API so the RM error code is visible in
 * dmesg instead of being silently dropped.
 */
static int gh_vmmgr_request_vmid(void)
{
	int vmid = 0;
	int ret;

	ret = gh_rm_vm_alloc_vmid(GH_OEM_VM, &vmid);
	if (ret) {
		pr_err("gh_rm_vm_alloc_vmid(GH_OEM_VM) failed: %d\n", ret);
		return ret;
	}

	if (vmid <= 0) {
		pr_err("RM returned invalid vmid %d\n", vmid);
		return -EINVAL;
	}

	gh_vmmgr_vmid = (unsigned int)vmid;
	pr_info("RM allocated vmid=%u\n", gh_vmmgr_vmid);
	return 0;
}

static int gh_vmmgr_release_vmid(void)
{
	int ret;

	if (!gh_vmmgr_registered)
		return 0;

	ret = gh_rm_vm_dealloc_vmid((gh_vmid_t)gh_vmmgr_vmid);
	if (ret)
		pr_warn("gh_rm_vm_dealloc_vmid(%u) failed: %d\n",
			gh_vmmgr_vmid, ret);
	else
		pr_info("RM released vmid=%u\n", gh_vmmgr_vmid);

	gh_vmmgr_registered = false;
	gh_vmmgr_vmid = 0;
	return ret;
}

static int gh_vmmgr_query_vminfo(struct gh_vmmgr_vminfo __user *info)
{
	struct gh_vmmgr_vminfo out;
	struct gh_vminfo rm_info;
	gh_vmid_t primary = GH_VMID_INVAL;
	int ret;

	memset(&out, 0, sizeof(out));
	memset(&rm_info, 0, sizeof(rm_info));

	ret = gh_rm_get_vmid(GH_PRIMARY_VM, &primary);
	if (ret) {
		pr_warn("gh_rm_get_vmid(primary) failed: %d\n", ret);
		primary = GH_VMID_INVAL;
	}
	out.primary_vmid = primary;

	ret = gh_rm_get_vminfo(GH_PRIMARY_VM, &rm_info);
	if (!ret) {
		out.has_uri = rm_info.uri && rm_info.uri[0];
		out.has_name = rm_info.name && rm_info.name[0];
	}

	if (gh_vmmgr_registered)
		out.allocated_vmid = gh_vmmgr_vmid;

	if (copy_to_user(info, &out, sizeof(out)))
		return -EFAULT;

	return 0;
}

static long gh_vmmgr_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret = -ENOTTY;

	mutex_lock(&gh_vmmgr_lock);

	switch (cmd) {
	case GH_VMMGR_IOC_QUERY_VMINFO:
		ret = gh_vmmgr_query_vminfo(argp);
		break;
	case GH_VMMGR_IOC_ALLOC_VMID:
		if (gh_vmmgr_registered) {
			ret = -EEXIST;
			break;
		}
		ret = gh_vmmgr_request_vmid();
		if (!ret)
			gh_vmmgr_registered = true;
		break;
	case GH_VMMGR_IOC_RELEASE_VMID:
		ret = gh_vmmgr_release_vmid();
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&gh_vmmgr_lock);
	return ret;
}

static int gh_vmmgr_open(struct inode *inode, struct file *filp)
{
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	return 0;
}

static int gh_vmmgr_release(struct inode *inode, struct file *filp)
{
	module_put(THIS_MODULE);
	return 0;
}

static const struct file_operations gh_vmmgr_fops = {
	.owner = THIS_MODULE,
	.open = gh_vmmgr_open,
	.release = gh_vmmgr_release,
	.unlocked_ioctl = gh_vmmgr_ioctl,
	.compat_ioctl = gh_vmmgr_ioctl,
	.llseek = no_llseek,
};

static int __init gh_vmmgr_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&gh_vmmgr_dev, 0, 1, GH_VMMGR_DEVICE_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&gh_vmmgr_cdev, &gh_vmmgr_fops);
	gh_vmmgr_cdev.owner = THIS_MODULE;
	ret = cdev_add(&gh_vmmgr_cdev, gh_vmmgr_dev, 1);
	if (ret) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_unregister_chrdev;
	}

	gh_vmmgr_class = class_create(THIS_MODULE, GH_VMMGR_CLASS_NAME);
	if (IS_ERR(gh_vmmgr_class)) {
		ret = PTR_ERR(gh_vmmgr_class);
		pr_err("class_create failed: %d\n", ret);
		goto err_del_cdev;
	}

	device_create(gh_vmmgr_class, NULL, gh_vmmgr_dev, NULL,
		      GH_VMMGR_DEVICE_NAME);

	pr_info("Gunyah VM manager (additive) loaded\n");
	pr_info("device /dev/%s created; autoload is off by default\n",
		GH_VMMGR_DEVICE_NAME);

	if (probe_rm) {
		struct gh_vmmgr_vminfo probe;

		mutex_lock(&gh_vmmgr_lock);
		ret = gh_vmmgr_query_vminfo(&probe);
		mutex_unlock(&gh_vmmgr_lock);
		if (ret) {
			pr_warn("probe_rm: query failed: %d\n", ret);
		} else {
			pr_info("probe_rm: primary_vmid=%u uri=%u name=%u\n",
				probe.primary_vmid, probe.has_uri, probe.has_name);
		}
	}

	return 0;

err_del_cdev:
	cdev_del(&gh_vmmgr_cdev);
err_unregister_chrdev:
	unregister_chrdev_region(gh_vmmgr_dev, 1);
	return ret;
}

static void __exit gh_vmmgr_exit(void)
{
	if (gh_vmmgr_registered)
		gh_vmmgr_release_vmid();

	device_destroy(gh_vmmgr_class, gh_vmmgr_dev);
	class_destroy(gh_vmmgr_class);
	cdev_del(&gh_vmmgr_cdev);
	unregister_chrdev_region(gh_vmmgr_dev, 1);

	pr_info("Gunyah VM manager (additive) unloaded\n");
}

module_init(gh_vmmgr_init);
module_exit(gh_vmmgr_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("k60-gunyah contributors");
MODULE_DESCRIPTION("Additive Gunyah VM manager probe for Redmi K60 (mondrian)");
MODULE_VERSION("0.2.0");
