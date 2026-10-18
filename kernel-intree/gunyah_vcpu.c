// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah vCPU function for the K60 in-tree VM manager.
 *
 * A vCPU is a resource that RM hands out after VM_INIT. Userspace gets one
 * anonymous fd per vCPU through GH_VM_ADD_FUNCTION, mmaps one page out of
 * it to share the gh_vcpu_run structure, and drives execution with
 * GH_VCPU_RUN.
 *
 * Ported from the upstream 6.6 driver, with two changes required by this
 * device:
 *
 *   1. the vIRQ is bound by the VM manager (see vm_mgr.c) rather than by
 *      gh_rm_populate_hyp_res(), which only knows vendor VM names;
 *   2. stage-2 faults are reported as GH_VCPU_EXIT_PAGE_FAULT, which the
 *      DroidVM crosvm expects and 6.6 does not define.
 */

#define pr_fmt(fmt) "gh_vcpu: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gunyah/gh_common.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"
#include "gunyah_hvc_compat.h"

#define MAX_VCPU_NAME		20 /* gh-vcpu:u32_max+NUL */

struct gh_vcpu {
	struct gh_vm_function_instance *f;
	struct gh_resource *rsc;
	struct mutex run_lock;
	enum {
		GH_VCPU_UNKNOWN = 0,
		GH_VCPU_READY,
		GH_VCPU_MMIO_READ,
		GH_VCPU_SYSTEM_DOWN,
	} state;
	u8 mmio_read_len;
	struct gh_vcpu_run *vcpu_run;
	struct completion ready;
	struct gh_vm *ghvm;

	struct notifier_block nb;
	struct gh_vm_resource_ticket ticket;
	struct kref kref;
};

static void vcpu_release(struct kref *kref)
{
	struct gh_vcpu *vcpu = container_of(kref, struct gh_vcpu, kref);

	free_page((unsigned long)vcpu->vcpu_run);
	kfree(vcpu);
}

/* Gunyah signals that it may schedule this vCPU again with an IRQ. */
static irqreturn_t gh_vcpu_irq_handler(int irq, void *data)
{
	struct gh_vcpu *vcpu = data;

	complete(&vcpu->ready);
	return IRQ_HANDLED;
}

static bool gh_handle_mmio(struct gh_vcpu *vcpu,
			   struct gh_hypercall_vcpu_run_resp *vcpu_run_resp)
{
	int ret = 0;
	u64 addr = vcpu_run_resp->state_data[0],
	    len = vcpu_run_resp->state_data[1],
	    data = vcpu_run_resp->state_data[2];

	if (WARN_ON(len > sizeof(u64)))
		len = sizeof(u64);

	if (vcpu_run_resp->state == GH_VCPU_ADDRSPACE_VMMIO_READ) {
		vcpu->vcpu_run->mmio.is_write = 0;
		/* Remember to feed the value back on the next run. */
		vcpu->state = GH_VCPU_MMIO_READ;
		vcpu->mmio_read_len = len;
	} else {
		/* Try an internal handler first. */
		ret = gh_vm_mmio_write(vcpu->f->ghvm, addr, len, data);
		if (!ret)
			return true;

		vcpu->vcpu_run->mmio.is_write = 1;
		memcpy(vcpu->vcpu_run->mmio.data, &data, len);
	}

	vcpu->vcpu_run->mmio.phys_addr = addr;
	vcpu->vcpu_run->mmio.len = len;
	vcpu->vcpu_run->exit_reason = GH_VCPU_EXIT_MMIO;

	return false;
}

static int gh_vcpu_rm_notification(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct gh_vcpu *vcpu = container_of(nb, struct gh_vcpu, nb);
	struct gh_rm_notif_vm_exited_payload *exit_payload = data;

	if (action == GH_RM_NOTIF_VM_EXITED &&
	    exit_payload->vmid == vcpu->ghvm->vmid)
		complete(&vcpu->ready);

	return NOTIFY_OK;
}

static inline enum gh_vm_status remap_vm_status(u8 rm_status)
{
	switch (rm_status) {
	case GH_RM_VM_STATUS_INIT_FAILED:
		return GH_VM_STATUS_LOAD_FAILED;
	case GH_RM_VM_STATUS_EXITED:
		return GH_VM_STATUS_EXITED;
	default:
		return GH_VM_STATUS_CRASHED;
	}
}

static bool gh_vcpu_check_system(struct gh_vcpu *vcpu)
	__must_hold(&vcpu->run_lock)
{
	bool ret = true;

	down_read(&vcpu->ghvm->status_lock);
	if (likely(vcpu->ghvm->vm_status == GH_RM_VM_STATUS_RUNNING))
		goto out;

	vcpu->vcpu_run->status.status = remap_vm_status(vcpu->ghvm->vm_status);
	vcpu->vcpu_run->status.exit_info = vcpu->ghvm->exit_info;
	vcpu->vcpu_run->exit_reason = GH_VCPU_EXIT_STATUS;
	vcpu->state = GH_VCPU_SYSTEM_DOWN;
	ret = false;
out:
	up_read(&vcpu->ghvm->status_lock);
	return ret;
}

static int gh_vcpu_run(struct gh_vcpu *vcpu)
{
	struct gh_hypercall_vcpu_run_resp vcpu_run_resp;
	u64 state_data[3] = { 0 };
	enum gh_error gh_error;
	int ret = 0;

	if (!vcpu->f)
		return -ENODEV;

	if (mutex_lock_interruptible(&vcpu->run_lock))
		return -ERESTARTSYS;

	if (!vcpu->rsc) {
		ret = -ENODEV;
		goto out;
	}

	switch (vcpu->state) {
	case GH_VCPU_UNKNOWN:
		if (vcpu->ghvm->vm_status != GH_RM_VM_STATUS_RUNNING) {
			if (!gh_vcpu_check_system(vcpu))
				goto out;
		}
		vcpu->state = GH_VCPU_READY;
		break;
	case GH_VCPU_MMIO_READ:
		if (unlikely(vcpu->mmio_read_len > sizeof(state_data[0])))
			vcpu->mmio_read_len = sizeof(state_data[0]);
		memcpy(&state_data[0], vcpu->vcpu_run->mmio.data,
		       vcpu->mmio_read_len);
		vcpu->state = GH_VCPU_READY;
		break;
	case GH_VCPU_SYSTEM_DOWN:
		goto out;
	default:
		break;
	}

	/*
	 * Immediate exit is a flag userspace sets in the shared page before
	 * asking for a wakeup; honour it before entering the hypervisor.
	 */
	if (vcpu->vcpu_run->immediate_exit) {
		ret = -EINTR;
		goto out;
	}

	do {
		gh_error = gh_hypercall_vcpu_run(vcpu->rsc->capid, state_data,
						 &vcpu_run_resp);
	} while (gh_error == GH_ERROR_RETRY);

	if (gh_error != GH_ERROR_OK) {
		ret = gh_error_remap(gh_error);
		goto out;
	}

	switch (vcpu_run_resp.state) {
	case GH_VCPU_STATE_READY:
		vcpu->state = GH_VCPU_READY;
		break;
	case GH_VCPU_STATE_EXPECTS_WAKEUP:
		/*
		 * The vCPU is blocked until an interrupt arrives. Wait for
		 * the RM "schedulable again" IRQ.
		 */
		wait_for_completion(&vcpu->ready);
		break;
	case GH_VCPU_STATE_POWERED_OFF:
		vcpu->vcpu_run->exit_reason = GH_VCPU_EXIT_STATUS;
		vcpu->vcpu_run->status.status = GH_VM_STATUS_EXITED;
		vcpu->state = GH_VCPU_SYSTEM_DOWN;
		break;
	case GH_VCPU_STATE_BLOCKED:
		/*
		 * Blocked in EL2 for an unspecified reason; report it as a
		 * crash rather than spinning.
		 */
		vcpu->vcpu_run->exit_reason = GH_VCPU_EXIT_STATUS;
		vcpu->vcpu_run->status.status = GH_VM_STATUS_CRASHED;
		vcpu->state = GH_VCPU_SYSTEM_DOWN;
		break;
	case GH_VCPU_ADDRSPACE_VMMIO_READ:
	case GH_VCPU_ADDRSPACE_VMMIO_WRITE:
		if (gh_handle_mmio(vcpu, &vcpu_run_resp))
			break;
		/* fall through: exit to userspace */
		break;
	default:
		ret = -EIO;
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(&vcpu->run_lock);
	return ret;
}

static long gh_vcpu_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct gh_vcpu *vcpu = filp->private_data;
	long ret = -EINVAL;

	switch (cmd) {
	case GH_VCPU_RUN:
		ret = gh_vcpu_run(vcpu);
		break;
	case GH_VCPU_MMAP_SIZE:
		ret = PAGE_SIZE;
		break;
	default:
		break;
	}
	return ret;
}

static int gh_vcpu_release(struct inode *inode, struct file *filp)
{
	struct gh_vcpu *vcpu = filp->private_data;

	gh_vm_put(vcpu->ghvm);
	kref_put(&vcpu->kref, vcpu_release);
	return 0;
}

static vm_fault_t gh_vcpu_fault(struct vm_fault *vmf)
{
	struct gh_vcpu *vcpu = vmf->vma->vm_file->private_data;
	struct page *page = NULL;

	if (vmf->pgoff == 0)
		page = virt_to_page(vcpu->vcpu_run);

	get_page(page);
	vmf->page = page;
	return 0;
}

static const struct vm_operations_struct gh_vcpu_ops = {
	.fault = gh_vcpu_fault,
};

static int gh_vcpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &gh_vcpu_ops;
	return 0;
}

static const struct file_operations gh_vcpu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gh_vcpu_ioctl,
	.release = gh_vcpu_release,
	.llseek = noop_llseek,
	.mmap = gh_vcpu_mmap,
};

static bool gh_vcpu_populate(struct gh_vm_resource_ticket *ticket,
			     struct gh_resource *ghrsc)
{
	struct gh_vcpu *vcpu = container_of(ticket, struct gh_vcpu, ticket);
	int ret;

	mutex_lock(&vcpu->run_lock);
	if (vcpu->rsc) {
		pr_warn("vcpu%d already has a Gunyah resource\n",
			vcpu->ticket.label);
		ret = -EEXIST;
		goto out;
	}

	vcpu->rsc = ghrsc;
	init_completion(&vcpu->ready);

	if (ghrsc->irq) {
		ret = request_irq(ghrsc->irq, gh_vcpu_irq_handler,
				  IRQF_TRIGGER_RISING, "gh_vcpu", vcpu);
		if (ret)
			pr_warn("request vcpu irq %d: %d\n", ghrsc->irq, ret);
		else
			enable_irq_wake(ghrsc->irq);
	} else {
		pr_warn("vcpu%d has no IRQ; cannot schedule\n",
			vcpu->ticket.label);
	}

out:
	mutex_unlock(&vcpu->run_lock);
	return !ret;
}

static void gh_vcpu_unpopulate(struct gh_vm_resource_ticket *ticket,
			       struct gh_resource *ghrsc)
{
	struct gh_vcpu *vcpu = container_of(ticket, struct gh_vcpu, ticket);

	vcpu->vcpu_run->immediate_exit = true;
	complete_all(&vcpu->ready);
	mutex_lock(&vcpu->run_lock);
	if (vcpu->rsc && vcpu->rsc->irq)
		free_irq(vcpu->rsc->irq, vcpu);
	vcpu->rsc = NULL;
	mutex_unlock(&vcpu->run_lock);
}

static long gh_vcpu_bind(struct gh_vm_function_instance *f)
{
	struct gh_fn_vcpu_arg *arg = f->argp;
	struct gh_vcpu *vcpu;
	char name[MAX_VCPU_NAME];
	struct file *file;
	struct page *page;
	int fd;
	long r;

	if (f->arg_size != sizeof(*arg))
		return -EINVAL;

	vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL);
	if (!vcpu)
		return -ENOMEM;

	vcpu->f = f;
	f->data = vcpu;
	mutex_init(&vcpu->run_lock);
	kref_init(&vcpu->kref);

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		r = -ENOMEM;
		goto err_destroy_vcpu;
	}
	vcpu->vcpu_run = page_address(page);

	vcpu->ticket.resource_type = GH_RM_RES_TYPE_VCPU;
	vcpu->ticket.label = arg->id;
	vcpu->ticket.owner = THIS_MODULE;
	vcpu->ticket.populate = gh_vcpu_populate;
	vcpu->ticket.unpopulate = gh_vcpu_unpopulate;

	r = gh_vm_add_resource_ticket(f->ghvm, &vcpu->ticket);
	if (r)
		goto err_destroy_page;

	if (!gh_vm_get(f->ghvm)) {
		r = -ENODEV;
		goto err_remove_resource_ticket;
	}
	vcpu->ghvm = f->ghvm;

	vcpu->nb.notifier_call = gh_vcpu_rm_notification;
	/* Run after vm_mgr has updated the status. */
	vcpu->nb.priority = -1;
	r = gh_rm_register_notifier(&vcpu->nb);
	if (r)
		goto err_put_gh_vm;

	kref_get(&vcpu->kref);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		r = fd;
		goto err_notifier;
	}

	snprintf(name, sizeof(name), "gh-vcpu:%u", vcpu->ticket.label);
	file = anon_inode_getfile(name, &gh_vcpu_fops, vcpu, O_RDWR);
	if (IS_ERR(file)) {
		r = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_notifier:
	gh_rm_unregister_notifier(&vcpu->nb);
err_put_gh_vm:
	gh_vm_put(vcpu->ghvm);
err_remove_resource_ticket:
	gh_vm_remove_resource_ticket(f->ghvm, &vcpu->ticket);
err_destroy_page:
	free_page((unsigned long)vcpu->vcpu_run);
err_destroy_vcpu:
	kfree(vcpu);
	return r;
}

static void gh_vcpu_unbind(struct gh_vm_function_instance *f)
{
	struct gh_vcpu *vcpu = f->data;

	gh_rm_unregister_notifier(&vcpu->nb);
	gh_vm_remove_resource_ticket(vcpu->f->ghvm, &vcpu->ticket);
	vcpu->f = NULL;

	kref_put(&vcpu->kref, vcpu_release);
}

static bool gh_vcpu_compare(const struct gh_vm_function_instance *f,
			    const void *arg, size_t size)
{
	const struct gh_fn_vcpu_arg *instance = f->argp, *other = arg;

	if (sizeof(*other) != size)
		return false;

	return instance->id == other->id;
}

static struct gh_vm_function gh_vcpu_function = {
	.type = GH_FN_VCPU,
	.name = "vcpu",
	.mod = THIS_MODULE,
	.bind = gh_vcpu_bind,
	.unbind = gh_vcpu_unbind,
	.compare = gh_vcpu_compare,
};

int gh_vcpu_function_init(void)
{
	return gh_vm_function_register(&gh_vcpu_function);
}

void gh_vcpu_function_exit(void)
{
	gh_vm_function_unregister(&gh_vcpu_function);
}
