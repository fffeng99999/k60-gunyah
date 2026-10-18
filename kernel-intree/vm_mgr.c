// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah VM manager for the Redmi K60 (mondrian, SM8475).
 *
 * A separate module (CONFIG_GH_VM_MGR=m). GH_VM_MGR depends on
 * GH_RM_DRV, which the device config sets to =m, so =y is not a
 * legal value here. The vendor gh_arm_drv / gh_msgq / gh_dbl /
 * gh_ctrl / gh_virtio_backend drivers are untouched and keep
 * serving DRM, qrtr and the virtio backends.
 *
 * The UABI exposed here is the crosvm "droidvm" ABI (see the uapi header in
 * this directory). Numbering is fixed by the shipped crosvm binary and must
 * not be changed.
 *
 * Device-specific constraint, and the reason this file exists at all: the
 * 5.10 vendor RM has no VM_CONFIG_IMAGE / VM_INIT / VM_GET_VMID /
 * VM_SET_FIRMWARE_MEM messages, so the upstream 6.6 gh_vm_start() sequence
 * cannot be replayed here. Instead the guest image is handed over as an
 * ordinary memory parcel and the entry point is set with
 * GH_VM_SET_BOOT_CONTEXT, which upstream Gunyah added precisely because
 * some resource managers do not accept an image descriptor.
 */

#define pr_fmt(fmt) "gh_vm_mgr: " fmt

#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include <linux/gunyah/gh_common.h>
#include <linux/gunyah/gh_errno.h>
#include <linux/gunyah/gh_rm_drv.h>

#include <uapi/linux/gunyah.h>

#include "gh_rm_vmmgr.h"
#include "vm_mgr.h"
#include "gh_rm_msgid_ext.h"
#include "gh_vm_mgr_compat.h"

static void gh_vm_free(struct work_struct *work);
static void gh_vm_put(struct gh_vm *ghvm);
static long gh_vm_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg);

/* ------------------------------------------------------------------ */
/* memory parcels                                                      */
/* ------------------------------------------------------------------ */

static bool pages_are_mergeable(struct page *a, struct page *b)
{
	return page_to_pfn(a) + 1 == page_to_pfn(b);
}

static bool gh_vm_mem_overlap(struct gh_vm_mem *a, u64 addr, u64 size)
{
	u64 a_end = a->guest_phys_addr + (a->npages << PAGE_SHIFT);
	u64 end = addr + size;

	return a->guest_phys_addr < end && addr < a_end;
}

static struct gh_vm_mem *__gh_vm_mem_find_by_label(struct gh_vm *ghvm, u32 label)
	__must_hold(&ghvm->mm_lock)
{
	struct gh_vm_mem *mapping;

	list_for_each_entry(mapping, &ghvm->memory_mappings, list)
		if (mapping->parcel.label == label)
			return mapping;

	return NULL;
}

static void gh_vm_mem_reclaim_mapping(struct gh_vm *ghvm,
				      struct gh_vm_mem *mapping)
	__must_hold(&ghvm->mm_lock)
{
	int ret = 0;

	if (mapping->parcel.mem_handle != GH_MEM_HANDLE_INVAL) {
		ret = gh_rm_mem_reclaim(mapping->parcel.mem_handle,
					GH_RM_MEM_RECLAIM_CLEAR);
		if (ret)
			pr_warn("reclaim parcel label %d: %d\n",
				mapping->parcel.label, ret);
	}

	if (!ret) {
		unpin_user_pages(mapping->pages, mapping->npages);
		account_locked_vm(ghvm->mm, mapping->npages, false);
	}

	kfree(mapping->pages);
	kfree(mapping->parcel.acl_entries);
	kfree(mapping->parcel.sgl_entries);
	kfree(mapping->parcel.mem_attr_entries);

	list_del(&mapping->list);
}

void gh_vm_mem_reclaim(struct gh_vm *ghvm)
{
	struct gh_vm_mem *mapping, *tmp;

	mutex_lock(&ghvm->mm_lock);
	list_for_each_entry_safe(mapping, tmp, &ghvm->memory_mappings, list) {
		gh_vm_mem_reclaim_mapping(ghvm, mapping);
		kfree(mapping);
	}
	mutex_unlock(&ghvm->mm_lock);
}

struct gh_vm_mem *gh_vm_mem_find_by_addr(struct gh_vm *ghvm,
					 u64 guest_phys_addr, u32 size)
{
	struct gh_vm_mem *mapping;

	if (guest_phys_addr + size < guest_phys_addr)
		return NULL;

	mutex_lock(&ghvm->mm_lock);
	list_for_each_entry(mapping, &ghvm->memory_mappings, list) {
		if (gh_vm_mem_overlap(mapping, guest_phys_addr, size))
			goto unlock;
	}

	mapping = NULL;
unlock:
	mutex_unlock(&ghvm->mm_lock);
	return mapping;
}

/*
 * Hand one parcel over to RM.
 *
 * The vendor gh_rm_mem_share()/gh_rm_mem_lend() take descriptor structs and
 * do the RPC framing themselves, so use those rather than re-implementing
 * the wire format.
 */
static int gh_vm_mem_parcel_out(struct gh_vm *ghvm, struct gh_vm_mem *mapping)
{
	struct gh_acl_desc *acl_desc;
	struct gh_sgl_desc *sgl_desc;
	struct gh_mem_attr_desc *attr_desc;
	gh_vmid_t host_vmid = GH_VMID_INVAL;
	u32 n_sgl;
	int i, ret;

	acl_desc = kzalloc(sizeof(*acl_desc) +
			   mapping->parcel.n_acl_entries *
			   sizeof(struct gh_acl_entry), GFP_KERNEL);
	if (!acl_desc)
		return -ENOMEM;

	sgl_desc = kzalloc(sizeof(*sgl_desc) +
			   mapping->parcel.n_sgl_entries *
			   sizeof(struct gh_sgl_entry), GFP_KERNEL);
	if (!sgl_desc) {
		ret = -ENOMEM;
		goto free_acl;
	}

	attr_desc = kzalloc(sizeof(*attr_desc), GFP_KERNEL);
	if (!attr_desc) {
		ret = -ENOMEM;
		goto free_sgl;
	}

	/*
	 * The vendor descriptors and the VM manager parcel entries are
	 * layout-identical but distinct types, so copy element by element.
	 * This also keeps the compiler honest if either side ever changes.
	 */
	for (i = 0; i < mapping->parcel.n_acl_entries; i++) {
		acl_desc->acl_entries[i].vmid =
			mapping->parcel.acl_entries[i].vmid;
		acl_desc->acl_entries[i].perms =
			mapping->parcel.acl_entries[i].perms;
		acl_desc->acl_entries[i].reserved =
			mapping->parcel.acl_entries[i].reserved;
	}
	acl_desc->n_acl_entries = mapping->parcel.n_acl_entries;

	for (i = 0; i < mapping->parcel.n_sgl_entries; i++) {
		sgl_desc->sgl_entries[i].ipa_base =
			mapping->parcel.sgl_entries[i].ipa_base;
		sgl_desc->sgl_entries[i].size =
			mapping->parcel.sgl_entries[i].size;
	}
	sgl_desc->n_sgl_entries = mapping->parcel.n_sgl_entries;
	n_sgl = mapping->parcel.n_sgl_entries;

	if (mapping->share_type == VM_MEM_SHARE) {
		ret = gh_rm_get_vmid(GH_PRIMARY_VM, &host_vmid);
		if (ret) {
			pr_warn("gh_rm_get_vmid(primary) failed: %d\n", ret);
			goto free_attr;
		}
	}

	if (mapping->share_type == VM_MEM_SHARE) {
		ret = gh_rm_mem_share(GH_RM_MEM_TYPE_NORMAL,
				      GH_RM_MEM_SHARE_SANITIZE,
				      mapping->parcel.label, acl_desc,
				      sgl_desc, attr_desc,
				      &mapping->parcel.mem_handle);
	} else {
		ret = gh_rm_mem_lend(GH_RM_MEM_TYPE_NORMAL,
				     GH_RM_MEM_LEND_SANITIZE,
				     mapping->parcel.label, acl_desc,
				     sgl_desc, attr_desc,
				     &mapping->parcel.mem_handle);
	}

	if (ret) {
		pr_warn("failed to %s parcel %d: %d\n",
			mapping->share_type == VM_MEM_LEND ? "lend" : "share",
			mapping->parcel.label, ret);
		goto free_attr;
	}

	/*
	 * Tell RM the recipient so it routes the notification. The vendor
	 * gh_rm_mem_notify() builds its own request, so only the handle and
	 * the flag matter here.
	 */
	{
		struct gh_notify_vmid_desc *vmid_desc;

		vmid_desc = kzalloc(sizeof(*vmid_desc) +
				    sizeof(struct gh_notify_vmid_entry),
				    GFP_KERNEL);
		if (vmid_desc) {
			vmid_desc->n_vmid_entries = 1;
			vmid_desc->vmid_entries[0].vmid = ghvm->vmid;
			gh_rm_mem_notify(mapping->parcel.mem_handle,
					 GH_RM_MEM_NOTIFY_RECIPIENT_SHARED,
					 mapping->parcel.label, vmid_desc);
			kfree(vmid_desc);
		}
	}

	for (i = 0; i < n_sgl; i++)
		pr_debug("parcel %d sgl[%d] ipa=%#llx size=%#llx\n",
			 mapping->parcel.label, i,
			 mapping->parcel.sgl_entries[i].ipa_base,
			 mapping->parcel.sgl_entries[i].size);

	ret = 0;
free_attr:
	kfree(attr_desc);
free_sgl:
	kfree(sgl_desc);
free_acl:
	kfree(acl_desc);
	return ret;
}

int gh_vm_mem_alloc(struct gh_vm *ghvm,
		    struct gh_userspace_memory_region *region, bool lend)
{
	struct gh_vm_mem *mapping, *tmp_mapping;
	struct page *curr_page, *prev_page;
	struct gh_rm_mem_parcel *parcel;
	int i, j, pinned, ret = 0;
	unsigned int gup_flags;
	size_t entry_size;
	gh_vmid_t host_vmid;

	if (!region->memory_size || !PAGE_ALIGNED(region->memory_size) ||
	    !PAGE_ALIGNED(region->userspace_addr) ||
	    !PAGE_ALIGNED(region->guest_phys_addr))
		return -EINVAL;

	if (region->guest_phys_addr + region->memory_size <
	    region->guest_phys_addr)
		return -EOVERFLOW;

	ret = mutex_lock_interruptible(&ghvm->mm_lock);
	if (ret)
		return ret;

	mapping = __gh_vm_mem_find_by_label(ghvm, region->label);
	if (mapping) {
		ret = -EEXIST;
		goto unlock;
	}

	list_for_each_entry(tmp_mapping, &ghvm->memory_mappings, list) {
		if (gh_vm_mem_overlap(tmp_mapping, region->guest_phys_addr,
				      region->memory_size)) {
			ret = -EEXIST;
			goto unlock;
		}
	}

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL_ACCOUNT);
	if (!mapping) {
		ret = -ENOMEM;
		goto unlock;
	}

	mapping->guest_phys_addr = region->guest_phys_addr;
	mapping->npages = region->memory_size >> PAGE_SHIFT;
	parcel = &mapping->parcel;
	parcel->label = region->label;
	parcel->mem_handle = GH_MEM_HANDLE_INVAL;
	parcel->mem_type = GH_RM_MEM_TYPE_NORMAL;
	parcel->n_acl_entries = lend ? 1 : 2;
	parcel->n_mem_attr_entries = 0;

	ret = account_locked_vm(ghvm->mm, mapping->npages, true);
	if (ret)
		goto free_mapping;

	mapping->pages = kcalloc(mapping->npages, sizeof(*mapping->pages),
				 GFP_KERNEL_ACCOUNT);
	if (!mapping->pages) {
		ret = -ENOMEM;
		mapping->npages = 0;
		goto unlock_pages;
	}

	gup_flags = FOLL_LONGTERM;
	if (region->flags & GH_MEM_ALLOW_WRITE)
		gup_flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast(region->userspace_addr, mapping->npages,
				     gup_flags, mapping->pages);
	if (pinned < 0) {
		ret = pinned;
		goto free_pages;
	} else if (pinned != mapping->npages) {
		ret = -EFAULT;
		mapping->npages = pinned;
		goto unpin_pages;
	}

	parcel->acl_entries = kcalloc(parcel->n_acl_entries,
				      sizeof(*parcel->acl_entries), GFP_KERNEL);
	if (!parcel->acl_entries) {
		ret = -ENOMEM;
		goto unpin_pages;
	}

	if (region->flags & GH_MEM_ALLOW_READ)
		parcel->acl_entries[0].perms |= GH_RM_ACL_R;
	if (region->flags & GH_MEM_ALLOW_WRITE)
		parcel->acl_entries[0].perms |= GH_RM_ACL_W;
	if (region->flags & GH_MEM_ALLOW_EXEC)
		parcel->acl_entries[0].perms |= GH_RM_ACL_X;

	if (lend) {
		mapping->share_type = VM_MEM_LEND;
	} else {
		ret = gh_rm_get_vmid(GH_PRIMARY_VM, &host_vmid);
		if (ret) {
			pr_warn("gh_rm_get_vmid(primary) failed: %d\n", ret);
			goto free_acl;
		}
		mapping->share_type = VM_MEM_SHARE;
		parcel->acl_entries[1].vmid = host_vmid;
		parcel->acl_entries[1].perms = GH_RM_ACL_R | GH_RM_ACL_W |
					      GH_RM_ACL_X;
	}

	/* Collapse contiguous pages into single SGL entries. */
	parcel->n_sgl_entries = 1;
	for (i = 1; i < mapping->npages; i++) {
		if (!pages_are_mergeable(mapping->pages[i - 1],
					 mapping->pages[i]))
			parcel->n_sgl_entries++;
	}

	parcel->sgl_entries = kcalloc(parcel->n_sgl_entries,
				      sizeof(*parcel->sgl_entries), GFP_KERNEL);
	if (!parcel->sgl_entries) {
		ret = -ENOMEM;
		goto free_acl;
	}

	prev_page = mapping->pages[0];
	parcel->sgl_entries[0].ipa_base = page_to_phys(prev_page);
	entry_size = PAGE_SIZE;
	for (i = 1, j = 0; i < mapping->npages; i++) {
		curr_page = mapping->pages[i];
		if (pages_are_mergeable(prev_page, curr_page)) {
			entry_size += PAGE_SIZE;
		} else {
			parcel->sgl_entries[j].size = entry_size;
			j++;
			parcel->sgl_entries[j].ipa_base =
				page_to_phys(curr_page);
			entry_size = PAGE_SIZE;
		}

		prev_page = curr_page;
	}
	parcel->sgl_entries[j].size = entry_size;

	list_add(&mapping->list, &ghvm->memory_mappings);
	mutex_unlock(&ghvm->mm_lock);
	return 0;

free_acl:
	kfree(parcel->acl_entries);
unpin_pages:
	unpin_user_pages(mapping->pages, pinned);
free_pages:
	kfree(mapping->pages);
unlock_pages:
	account_locked_vm(ghvm->mm, mapping->npages, false);
free_mapping:
	kfree(mapping);
unlock:
	mutex_unlock(&ghvm->mm_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* hypervisor resources                                                */
/* ------------------------------------------------------------------ */

static struct gh_resource *gh_vmmgr_alloc_resource(u32 label, u32 res_type,
						   u64 cap_id, u32 virq_handle,
						   s32 virq, u64 base, u64 size)
{
	struct gh_resource *ghrsc;

	ghrsc = kzalloc(sizeof(*ghrsc), GFP_KERNEL);
	if (!ghrsc)
		return ERR_PTR(-ENOMEM);

	ghrsc->type = res_type;
	ghrsc->rm_label = label;
	ghrsc->capid = cap_id;
	ghrsc->virq_handle = virq_handle;
	ghrsc->virq = virq;
	ghrsc->base = base;
	ghrsc->size = size;
	ghrsc->irq = 0;

	return ghrsc;
}

static int gh_vm_fetch_resources(struct gh_vm *ghvm)
{
	struct gh_vmmgr_hyp_res_payload *resp;
	struct gh_vmmgr_hyp_res_entry *entry;
	struct gh_resource *ghrsc;
	size_t resp_size;
	u32 n, i;
	int ret;

	ret = gh_rm_vm_get_hyp_resources_raw(ghvm->vmid,
					     (void **)&resp, &resp_size);
	if (ret)
		return ret;

	if (resp_size < sizeof(*resp) ||
	    resp_size != sizeof(*resp) +
			 le32_to_cpu(resp->n_resource_entries) *
			 sizeof(*entry)) {
		pr_err("VM_GET_HYP_RESOURCES(%u): bad size %zu\n",
		       ghvm->vmid, resp_size);
		kfree(resp);
		return -EBADMSG;
	}

	n = le32_to_cpu(resp->n_resource_entries);
	pr_info("VM %u has %u hypervisor resources\n", ghvm->vmid, n);

	for (i = 0; i < n; i++) {
		u64 cap_id, base, size;

		entry = &resp->entries[i];
		cap_id = ((u64)le32_to_cpu(entry->cap_id_high) << 32) |
			 le32_to_cpu(entry->cap_id_low);
		base = ((u64)le32_to_cpu(entry->base_high) << 32) |
		       le32_to_cpu(entry->base_low);
		size = ((u64)le32_to_cpu(entry->size_high) << 32) |
		       le32_to_cpu(entry->size_low);

		ghrsc = gh_vmmgr_alloc_resource(
			le32_to_cpu(entry->resource_label),
			entry->res_type, cap_id,
			le32_to_cpu(entry->virq_handle),
			le32_to_cpu(entry->virq), base, size);
		if (IS_ERR(ghrsc)) {
			ret = PTR_ERR(ghrsc);
			goto out;
		}

		/*
		 * Bind the vIRQ here rather than going through
		 * gh_rm_populate_hyp_res(): that function dispatches by
		 * enum gh_vm_names, and a VMID we allocated ourselves has no
		 * entry in the vendor gh_vm_table.
		 */
		if (entry->res_type != GH_RM_RES_TYPE_DB_TX &&
		    entry->virq_handle && entry->virq) {
			int linux_irq;

			linux_irq = gh_rm_vm_bind_virq(
				le32_to_cpu(entry->virq_handle),
				le32_to_cpu(entry->virq));
			if (linux_irq < 0) {
				pr_warn("bind virq handle=%u virq=%d: %d\n",
					le32_to_cpu(entry->virq_handle),
					le32_to_cpu(entry->virq), linux_irq);
				ghrsc->irq = 0;
			} else {
				ghrsc->irq = linux_irq;
			}
		}

		pr_info("  res type=%u label=%u cap=%#llx virq=%u irq=%d base=%#llx size=%#llx\n",
			entry->res_type, le32_to_cpu(entry->resource_label),
			cap_id, le32_to_cpu(entry->virq), ghrsc->irq,
			base, size);

		list_add(&ghrsc->list, &ghvm->resources);
	}

	ret = 0;
out:
	kfree(resp);
	return ret;
}

static void gh_vm_free_resources(struct gh_vm *ghvm)
{
	struct gh_resource *ghrsc, *tmp;

	list_for_each_entry_safe(ghrsc, tmp, &ghvm->resources, list) {
		if (ghrsc->virq_handle && ghrsc->irq)
			gh_rm_vm_unbind_virq(ghrsc->virq_handle, ghrsc->irq);
		list_del(&ghrsc->list);
		kfree(ghrsc);
	}
}

/* ------------------------------------------------------------------ */
/* resource tickets                                                    */
/* ------------------------------------------------------------------ */

int gh_vm_add_resource_ticket(struct gh_vm *ghvm,
			      struct gh_vm_resource_ticket *ticket)
{
	struct gh_vm_resource_ticket *iter;
	struct gh_resource *ghrsc, *rsc_iter;
	int ret = 0;

	mutex_lock(&ghvm->resources_lock);
	list_for_each_entry(iter, &ghvm->resource_tickets, vm_list) {
		if (iter->resource_type == ticket->resource_type &&
		    iter->label == ticket->label) {
			ret = -EEXIST;
			goto out;
		}
	}

	if (!try_module_get(ticket->owner)) {
		ret = -ENODEV;
		goto out;
	}

	list_add(&ticket->vm_list, &ghvm->resource_tickets);
	INIT_LIST_HEAD(&ticket->resources);

	list_for_each_entry_safe(ghrsc, rsc_iter, &ghvm->resources, list) {
		if (ghrsc->type == ticket->resource_type &&
		    ghrsc->rm_label == ticket->label) {
			if (ticket->populate(ticket, ghrsc))
				list_move(&ghrsc->list, &ticket->resources);
		}
	}
out:
	mutex_unlock(&ghvm->resources_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gh_vm_add_resource_ticket);

void gh_vm_remove_resource_ticket(struct gh_vm *ghvm,
				  struct gh_vm_resource_ticket *ticket)
{
	struct gh_resource *ghrsc, *riter;

	mutex_lock(&ghvm->resources_lock);
	if (ticket->unpopulate) {
		list_for_each_entry_safe(ghrsc, riter, &ticket->resources,
					 list)
			ticket->unpopulate(ticket, ghrsc);
	}
	list_for_each_entry_safe(ghrsc, riter, &ticket->resources, list)
		list_del(&ghrsc->list);
	list_del(&ticket->vm_list);
	module_put(ticket->owner);
	mutex_unlock(&ghvm->resources_lock);
}
EXPORT_SYMBOL_GPL(gh_vm_remove_resource_ticket);

/* ------------------------------------------------------------------ */
/* MMIO dispatch                                                       */
/* ------------------------------------------------------------------ */

/*
 * MMIO handlers are kept on a simple list.
 *
 * Upstream 6.6 uses an rbtree here through rb_find()/rb_find_add(), but
 * neither helper exists in 5.10 (they arrived later). A VMM registers a
 * handful of ioeventfds, so a linear scan is both sufficient and one less
 * kernel-version dependency to get wrong.
 */
static bool gh_vm_io_handler_matches(const struct gh_vm_io_handler *h,
				     u64 addr, u32 len, u64 data)
{
	if (h->addr != addr)
		return false;

	/* len 0 means "any length". */
	if (h->len && len && h->len != len)
		return false;

	if (h->datamatch && h->data != data)
		return false;

	return true;
}

int gh_vm_mmio_write(struct gh_vm *ghvm, u64 addr, u32 len, u64 data)
{
	struct gh_vm_io_handler *io_hdlr;
	int ret = -ENODEV;

	down_read(&ghvm->mmio_handler_lock);
	list_for_each_entry(io_hdlr, &ghvm->mmio_handlers, node) {
		if (!gh_vm_io_handler_matches(io_hdlr, addr, len, data))
			continue;
		if (!io_hdlr->ops || !io_hdlr->ops->write)
			continue;

		ret = io_hdlr->ops->write(io_hdlr, addr, len, data);
		break;
	}
	up_read(&ghvm->mmio_handler_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(gh_vm_mmio_write);

int gh_vm_add_io_handler(struct gh_vm *ghvm, struct gh_vm_io_handler *io_hdlr)
{
	struct gh_vm_io_handler *iter;
	int ret = 0;

	if (io_hdlr->datamatch && (!io_hdlr->len ||
				   io_hdlr->len > sizeof(io_hdlr->data)))
		return -EINVAL;

	down_write(&ghvm->mmio_handler_lock);
	list_for_each_entry(iter, &ghvm->mmio_handlers, node) {
		if (gh_vm_io_handler_matches(iter, io_hdlr->addr,
					     io_hdlr->len, io_hdlr->data) &&
		    iter->datamatch == io_hdlr->datamatch) {
			ret = -EEXIST;
			goto out;
		}
	}

	list_add_tail(&io_hdlr->node, &ghvm->mmio_handlers);
out:
	up_write(&ghvm->mmio_handler_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gh_vm_add_io_handler);

void gh_vm_remove_io_handler(struct gh_vm *ghvm,
			     struct gh_vm_io_handler *io_hdlr)
{
	down_write(&ghvm->mmio_handler_lock);
	list_del(&io_hdlr->node);
	up_write(&ghvm->mmio_handler_lock);
}
EXPORT_SYMBOL_GPL(gh_vm_remove_io_handler);

/* ------------------------------------------------------------------ */
/* VM functions (vCPU, irqfd, ioeventfd)                               */
/* ------------------------------------------------------------------ */

static DEFINE_XARRAY(gh_vm_functions);

static void gh_vm_put_function(struct gh_vm_function *fn)
{
	module_put(fn->mod);
}

static struct gh_vm_function *gh_vm_get_function(u32 type)
{
	struct gh_vm_function *fn;

	fn = xa_load(&gh_vm_functions, type);
	if (!fn) {
		request_module("ghfunc:%d", type);

		fn = xa_load(&gh_vm_functions, type);
	}

	if (!fn || !try_module_get(fn->mod))
		fn = ERR_PTR(-ENOENT);

	return fn;
}

int gh_vm_function_register(struct gh_vm_function *fn)
{
	if (!fn->bind || !fn->unbind)
		return -EINVAL;

	return xa_err(xa_store(&gh_vm_functions, fn->type, fn, GFP_KERNEL));
}
EXPORT_SYMBOL_GPL(gh_vm_function_register);

void gh_vm_function_unregister(struct gh_vm_function *fn)
{
	WARN_ON(fn->mod && module_refcount(fn->mod));
	xa_erase(&gh_vm_functions, fn->type);
}
EXPORT_SYMBOL_GPL(gh_vm_function_unregister);

static void gh_vm_remove_function_instance(struct gh_vm_function_instance *inst)
	__must_hold(&inst->ghvm->fn_lock)
{
	inst->fn->unbind(inst);
	list_del(&inst->vm_list);
	gh_vm_put_function(inst->fn);
	kfree(inst->argp);
	kfree(inst);
}

static void gh_vm_remove_functions(struct gh_vm *ghvm)
{
	struct gh_vm_function_instance *inst, *iiter;

	mutex_lock(&ghvm->fn_lock);
	list_for_each_entry_safe(inst, iiter, &ghvm->functions, vm_list)
		gh_vm_remove_function_instance(inst);
	mutex_unlock(&ghvm->fn_lock);
}

static long gh_vm_add_function_instance(struct gh_vm *ghvm,
					struct gh_fn_desc *f)
{
	struct gh_vm_function_instance *inst;
	void __user *argp;
	long r = 0;

	if (f->arg_size > GH_FN_MAX_ARG_SIZE) {
		pr_err_ratelimited("%s: arg_size > %d\n",
				    __func__, GH_FN_MAX_ARG_SIZE);
		return -EINVAL;
	}

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->arg_size = f->arg_size;
	if (inst->arg_size) {
		inst->argp = kzalloc(inst->arg_size, GFP_KERNEL);
		if (!inst->argp) {
			r = -ENOMEM;
			goto free;
		}

		argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(inst->argp, argp, f->arg_size)) {
			r = -EFAULT;
			goto free_arg;
		}
	}

	inst->fn = gh_vm_get_function(f->type);
	if (IS_ERR(inst->fn)) {
		r = PTR_ERR(inst->fn);
		goto free_arg;
	}

	inst->ghvm = ghvm;

	mutex_lock(&ghvm->fn_lock);
	r = inst->fn->bind(inst);
	if (r < 0) {
		mutex_unlock(&ghvm->fn_lock);
		gh_vm_put_function(inst->fn);
		goto free_arg;
	}

	list_add(&inst->vm_list, &ghvm->functions);
	mutex_unlock(&ghvm->fn_lock);

	return r;
free_arg:
	kfree(inst->argp);
free:
	kfree(inst);
	return r;
}

static long gh_vm_rm_function_instance(struct gh_vm *ghvm,
				       struct gh_fn_desc *f)
{
	struct gh_vm_function_instance *inst, *iter;
	void __user *user_argp;
	void *argp;
	long r = 0;

	r = mutex_lock_interruptible(&ghvm->fn_lock);
	if (r)
		return r;

	if (f->arg_size) {
		argp = kzalloc(f->arg_size, GFP_KERNEL);
		if (!argp) {
			r = -ENOMEM;
			goto out;
		}

		user_argp = u64_to_user_ptr(f->arg);
		if (copy_from_user(argp, user_argp, f->arg_size)) {
			r = -EFAULT;
			kfree(argp);
			goto out;
		}

		r = -ENOENT;
		list_for_each_entry_safe(inst, iter, &ghvm->functions,
					 vm_list) {
			if (inst->fn->type == f->type &&
			    inst->fn->compare(inst, argp, f->arg_size)) {
				gh_vm_remove_function_instance(inst);
				r = 0;
			}
		}

		kfree(argp);
	}

out:
	mutex_unlock(&ghvm->fn_lock);
	return r;
}

/* ------------------------------------------------------------------ */
/* VM status notifications                                             */
/* ------------------------------------------------------------------ */

static int gh_vm_rm_notification_status(struct gh_vm *ghvm, void *data)
{
	struct gh_rm_notif_vm_status_payload *payload = data;

	if (payload->vmid != ghvm->vmid)
		return NOTIFY_OK;

	if (payload->vm_status == GH_RM_VM_STATUS_RESET ||
	    payload->vm_status == GH_RM_VM_STATUS_EXITED) {
		down_write(&ghvm->status_lock);
		ghvm->vm_status = payload->vm_status;
		up_write(&ghvm->status_lock);
		wake_up(&ghvm->vm_status_wait);
	}

	return NOTIFY_DONE;
}

static int gh_vm_rm_notification_exited(struct gh_vm *ghvm, void *data)
{
	struct gh_rm_notif_vm_exited_payload *payload = data;

	if (payload->vmid != ghvm->vmid)
		return NOTIFY_OK;

	down_write(&ghvm->status_lock);
	ghvm->vm_status = GH_RM_VM_STATUS_EXITED;
	ghvm->exit_info.type = payload->exit_type;
	ghvm->exit_info.reason_size = min_t(u32, payload->exit_reason_size,
					    GH_VM_MAX_EXIT_REASON_SIZE);
	memcpy(&ghvm->exit_info.reason, payload->exit_reason,
	       ghvm->exit_info.reason_size);
	up_write(&ghvm->status_lock);
	wake_up(&ghvm->vm_status_wait);

	return NOTIFY_DONE;
}

static int gh_vm_rm_notification(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct gh_vm *ghvm = container_of(nb, struct gh_vm, nb);

	switch (action) {
	case GH_RM_NOTIF_VM_STATUS:
		return gh_vm_rm_notification_status(ghvm, data);
	case GH_RM_NOTIF_VM_EXITED:
		return gh_vm_rm_notification_exited(ghvm, data);
	default:
		return NOTIFY_OK;
	}
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                        */
/* ------------------------------------------------------------------ */

static __must_check struct gh_vm *gh_vm_alloc(void)
{
	struct gh_vm *ghvm;

	ghvm = kzalloc(sizeof(*ghvm), GFP_KERNEL);
	if (!ghvm)
		return ERR_PTR(-ENOMEM);

	ghvm->vmid = GH_VMID_INVAL;
	mutex_init(&ghvm->mm_lock);
	INIT_LIST_HEAD(&ghvm->memory_mappings);
	init_rwsem(&ghvm->status_lock);
	init_waitqueue_head(&ghvm->vm_status_wait);
	INIT_WORK(&ghvm->free_work, gh_vm_free);
	kref_init(&ghvm->kref);
	mutex_init(&ghvm->resources_lock);
	INIT_LIST_HEAD(&ghvm->resources);
	INIT_LIST_HEAD(&ghvm->resource_tickets);
	init_rwsem(&ghvm->mmio_handler_lock);
	INIT_LIST_HEAD(&ghvm->mmio_handlers);
	INIT_LIST_HEAD(&ghvm->functions);
	ghvm->vm_status = GH_RM_VM_STATUS_NO_STATE;

	mmgrab(current->mm);
	ghvm->mm = current->mm;

	return ghvm;
}

/*
 * Bring the VM up.
 *
 * The device RM has no VM_CONFIG_IMAGE, so the sequence is:
 *
 *   VM_ALLOCATE          -> vmid
 *   MEM_LEND / MEM_SHARE -> one parcel per userspace region
 *   VM_SET_BOOT_CONTEXT  -> guest entry point (Android addition)
 *   VM_INIT              -> RM builds stage-2 and hands out capabilities
 *   VM_GET_HYP_RESOURCES -> vCPU cap ids, doorbells
 *   VM_START             -> VM runs
 *
 * If RM refuses any step this fails cleanly and gh_vm_free() unwinds.
 */
static int gh_vm_start(struct gh_vm *ghvm)
{
	struct gh_vm_mem *mapping;
	int ret, i;

	/*
	 * Hold the write lock for the whole sequence. Without it two concurrent
	 * GH_VM_START ioctls could both observe NO_STATE and each allocate a
	 * VMID, leaking one.
	 */
	down_write(&ghvm->status_lock);

	if (ghvm->vm_status != GH_RM_VM_STATUS_NO_STATE) {
		up_write(&ghvm->status_lock);
		return 0;
	}

	ghvm->nb.notifier_call = gh_vm_rm_notification;
	ret = gh_rm_register_notifier(&ghvm->nb);
	if (ret) {
		up_write(&ghvm->status_lock);
		return ret;
	}

	ret = gh_rm_vm_alloc_vmid_any(&ghvm->vmid);
	if (ret < 0) {
		gh_rm_unregister_notifier(&ghvm->nb);
		up_write(&ghvm->status_lock);
		return ret;
	}
	ghvm->vm_status = GH_RM_VM_STATUS_INIT;
	pr_info("allocated vmid %u\n", ghvm->vmid);

	mutex_lock(&ghvm->mm_lock);
	list_for_each_entry(mapping, &ghvm->memory_mappings, list) {
		ret = gh_vm_mem_parcel_out(ghvm, mapping);
		if (ret) {
			mutex_unlock(&ghvm->mm_lock);
			pr_err("failed to hand over parcel %d: %d\n",
			       mapping->parcel.label, ret);
			goto err;
		}
	}
	mutex_unlock(&ghvm->mm_lock);

	/*
	 * VM_SET_BOOT_CONTEXT is not in the vendor message table, and this is
	 * a module so it cannot reach gh_rm_call() directly. The narrow wrapper
	 * in gh_rm_vmmgr.c owns the wire format; if this RM does not know the
	 * message we get a clean error and carry on rather than a hang.
	 */
	for (i = 0; i < ghvm->n_boot_ctx; i++) {
		ret = gh_rm_set_vm_boot_context(ghvm->vmid,
						ghvm->boot_ctx[i].reg,
						ghvm->boot_ctx[i].value);
		if (ret)
			pr_warn("boot context reg=%#x value=%#llx: %d\n",
				ghvm->boot_ctx[i].reg,
				ghvm->boot_ctx[i].value, ret);
		else
			pr_info("boot context reg=%#x value=%#llx accepted\n",
				ghvm->boot_ctx[i].reg,
				ghvm->boot_ctx[i].value);
	}

	ret = gh_rm_vm_init(ghvm->vmid);
	if (ret) {
		ghvm->vm_status = GH_RM_VM_STATUS_INIT_FAILED;
		pr_err("VM_INIT(%u) failed: %d\n", ghvm->vmid, ret);
		goto err;
	}
	ghvm->vm_status = GH_RM_VM_STATUS_READY;

	ret = gh_vm_fetch_resources(ghvm);
	if (ret) {
		pr_err("failed to fetch resources for VM %u: %d\n",
		       ghvm->vmid, ret);
		goto err;
	}

	ret = gh_rm_vm_start_raw(ghvm->vmid);
	if (ret) {
		pr_err("VM_START(%u) failed: %d\n", ghvm->vmid, ret);
		goto err;
	}

	ghvm->vm_status = GH_RM_VM_STATUS_RUNNING;
	pr_info("VM %u running\n", ghvm->vmid);
	up_write(&ghvm->status_lock);
	return 0;

err:
	/*
	 * Leave vm_status at INIT or INIT_FAILED. gh_vm_free() reads it to
	 * decide what to release: it skips VM_RESET, which RM rejects for a
	 * VM that never came up, but still deallocates the VMID. Clearing
	 * the state here would leak the VMID.
	 */
	up_write(&ghvm->status_lock);
	gh_vm_put(ghvm);
	return ret;
}

static int gh_vm_ensure_started(struct gh_vm *ghvm)
{
	int ret;

	ret = down_read_interruptible(&ghvm->status_lock);
	if (ret)
		return ret;

	if (unlikely(ghvm->vm_status == GH_RM_VM_STATUS_NO_STATE)) {
		up_read(&ghvm->status_lock);
		/*
		 * gh_vm_start() serialises on the write lock, so the status it
		 * leaves behind is final. Re-reading it once below is enough;
		 * recursing would turn a start failure into a confusing
		 * -ENODEV instead of the real error.
		 */
		ret = gh_vm_start(ghvm);
		if (ret)
			return ret;

		ret = down_read_interruptible(&ghvm->status_lock);
		if (ret)
			return ret;
	}

	if (unlikely(ghvm->vm_status != GH_RM_VM_STATUS_RUNNING))
		ret = -ENODEV;

	up_read(&ghvm->status_lock);
	return ret;
}

static void gh_vm_stop(struct gh_vm *ghvm)
{
	int ret;

	down_write(&ghvm->status_lock);
	if (ghvm->vm_status == GH_RM_VM_STATUS_RUNNING) {
		ret = gh_rm_vm_stop_raw(ghvm->vmid,
				    GH_VM_STOP_REASON_FORCE_STOP,
				    GH_RM_VM_STOP_FLAG_FORCE_STOP);
		if (ret)
			pr_warn("VM_STOP(%u) failed: %d\n", ghvm->vmid, ret);
	}
	up_write(&ghvm->status_lock);

	/*
	 * Bounded: RM should answer VM_STOP with an EXITED notification, but if
	 * it does not there is nothing to gain by blocking a workqueue thread
	 * forever. gh_vm_free() still tries a reset and a deallocate after this.
	 */
	if (!wait_event_timeout(ghvm->vm_status_wait,
				ghvm->vm_status == GH_RM_VM_STATUS_EXITED ||
				ghvm->vm_status == GH_RM_VM_STATUS_RESET,
				msecs_to_jiffies(5000)))
		pr_warn("VM %u: no exit notification within 5s\n", ghvm->vmid);
}

static void gh_vm_free(struct work_struct *work)
{
	struct gh_vm *ghvm = container_of(work, struct gh_vm, free_work);
	int ret;

	if (ghvm->vm_status == GH_RM_VM_STATUS_RUNNING)
		gh_vm_stop(ghvm);

	gh_vm_remove_functions(ghvm);
	gh_vm_free_resources(ghvm);

	/*
	 * Only reset a VM that actually reached a running or ready state.
	 * RM rejects VM_RESET for a VM it only allocated, which is the EPERM
	 * the tree-out-of-tree module hit in phase 2. Resetting there is not
	 * needed anyway: nothing was configured.
	 */
	if (ghvm->vm_status == GH_RM_VM_STATUS_RUNNING ||
	    ghvm->vm_status == GH_RM_VM_STATUS_READY ||
	    ghvm->vm_status == GH_RM_VM_STATUS_INIT_FAILED ||
	    ghvm->vm_status == GH_RM_VM_STATUS_EXITED) {
		ret = gh_rm_vm_reset_sync(ghvm->vmid);
		if (ret)
			pr_warn("reset VM %u: %d\n", ghvm->vmid, ret);
	}

	gh_vm_mem_reclaim(ghvm);

	if (ghvm->vm_status > GH_RM_VM_STATUS_NO_STATE) {
		gh_rm_unregister_notifier(&ghvm->nb);

		ret = gh_rm_vm_dealloc_vmid_any(ghvm->vmid);
		if (ret)
			pr_warn("deallocate VMID %u: %d\n",
				ghvm->vmid, ret);
		else
			pr_info("released vmid %u\n", ghvm->vmid);
	}

	mmdrop(ghvm->mm);
	kfree(ghvm);
}

int __must_check gh_vm_get(struct gh_vm *ghvm)
{
	return kref_get_unless_zero(&ghvm->kref);
}
EXPORT_SYMBOL_GPL(gh_vm_get);

static void _gh_vm_put(struct kref *kref)
{
	struct gh_vm *ghvm = container_of(kref, struct gh_vm, kref);

	/* RM calls sleep; defer so the closing task can still be signalled. */
	schedule_work(&ghvm->free_work);
}

static void gh_vm_put(struct gh_vm *ghvm)
{
	kref_put(&ghvm->kref, _gh_vm_put);
}

static int gh_vm_release(struct inode *inode, struct file *filp)
{
	struct gh_vm *ghvm = filp->private_data;

	gh_vm_put(ghvm);
	return 0;
}

static const struct file_operations gh_vm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gh_vm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.release = gh_vm_release,
	.llseek = noop_llseek,
};

/* ------------------------------------------------------------------ */
/* ioctls                                                             */
/* ------------------------------------------------------------------ */

static long gh_vm_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct gh_vm *ghvm = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r;
	bool lend = false;

	switch (cmd) {
	case GH_VM_ANDROID_LEND_USER_MEM:
		lend = true;
		fallthrough;
	case GH_VM_SET_USER_MEM_REGION: {
		struct gh_userspace_memory_region region;

		/* only the owning task may add memory */
		if (ghvm->mm != current->mm)
			return -EPERM;

		if (copy_from_user(&region, argp, sizeof(region)))
			return -EFAULT;

		if (region.flags & ~(GH_MEM_ALLOW_READ | GH_MEM_ALLOW_WRITE |
				     GH_MEM_ALLOW_EXEC))
			return -EINVAL;

		r = gh_vm_mem_alloc(ghvm, &region, lend);
		break;
	}
	case GH_VM_SET_DTB_CONFIG: {
		/*
		 * Stored for later use. The device RM has no VM_CONFIG_IMAGE,
		 * so there is nothing to hand the DTB to yet; the field exists
		 * so the ioctl succeeds and crosvm's setup sequence does not
		 * abort before reaching GH_VM_START.
		 */
		struct gh_vm_dtb_config dtb_config;

		if (copy_from_user(&dtb_config, argp, sizeof(dtb_config)))
			return -EFAULT;

		if (dtb_config.guest_phys_addr + dtb_config.size <
		    dtb_config.guest_phys_addr)
			return -EOVERFLOW;

		ghvm->dtb_config = dtb_config;
		r = 0;
		break;
	}
	case GH_VM_SET_BOOT_CONTEXT: {
		struct gh_vm_boot_context boot_ctx;

		if (copy_from_user(&boot_ctx, argp, sizeof(boot_ctx)))
			return -EFAULT;

		if (ghvm->n_boot_ctx >= GH_VM_MAX_BOOT_CTX)
			return -EINVAL;

		ghvm->boot_ctx[ghvm->n_boot_ctx].reg = boot_ctx.reg;
		ghvm->boot_ctx[ghvm->n_boot_ctx].value = boot_ctx.value;
		ghvm->n_boot_ctx++;
		r = 0;
		break;
	}
	case GH_VM_ANDROID_SET_FW_CONFIG: {
		/*
		 * Same as the DTB config: recorded so the ioctl succeeds, but
		 * the device RM has no VM_SET_FIRMWARE_MEM to send it to.
		 */
		struct gh_vm_firmware_config fw_config;

		if (copy_from_user(&fw_config, argp, sizeof(fw_config)))
			return -EFAULT;

		ghvm->fw_config = fw_config;
		ghvm->auth = GH_RM_VM_AUTH_QCOM_ANDROID_PVM;
		r = 0;
		break;
	}
	case GH_VM_START:
		r = gh_vm_ensure_started(ghvm);
		break;
	case GH_VM_ADD_FUNCTION: {
		struct gh_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gh_vm_add_function_instance(ghvm, &f);
		break;
	}
	case GH_VM_REMOVE_FUNCTION: {
		struct gh_fn_desc f;

		if (copy_from_user(&f, argp, sizeof(f)))
			return -EFAULT;

		r = gh_vm_rm_function_instance(ghvm, &f);
		break;
	}
	case GH_VM_RECLAIM_REGION: {
		struct gunyah_address_range range;
		struct gh_vm_mem *mapping;

		if (copy_from_user(&range, argp, sizeof(range)))
			return -EFAULT;

		/*
		 * crosvm reclaims a runtime SHARE by the deterministic label
		 * it used, which is the guest page number.
		 */
		mutex_lock(&ghvm->mm_lock);
		mapping = __gh_vm_mem_find_by_label(
			ghvm, (u32)(range.guest_phys_addr >> 12));
		if (!mapping) {
			mutex_unlock(&ghvm->mm_lock);
			r = -ENOENT;
			break;
		}
		gh_vm_mem_reclaim_mapping(ghvm, mapping);
		kfree(mapping);
		mutex_unlock(&ghvm->mm_lock);
		r = 0;
		break;
	}
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

static long gh_dev_ioctl_create_vm(unsigned long arg)
{
	struct gh_vm *ghvm;
	struct file *file;
	int fd, err;

	/* arg reserved for future use */
	if (arg)
		return -EINVAL;

	ghvm = gh_vm_alloc();
	if (IS_ERR(ghvm))
		return PTR_ERR(ghvm);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_destroy_vm;
	}

	file = anon_inode_getfile("gunyah-vm", &gh_vm_fops, ghvm, O_RDWR);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_destroy_vm:
	gh_vm_put(ghvm);
	return err;
}

static long gh_dev_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	switch (cmd) {
	case GH_CREATE_VM:
		return gh_dev_ioctl_create_vm(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations gh_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gh_dev_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice gh_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gunyah",
	.fops = &gh_dev_fops,
	/*
	 * 0660 so a member of the "gunyah" group can open it without root.
	 * The vendor sepolicy still has to permit the crosvm domain; that is
	 * handled separately in the Magisk module.
	 */
	.mode = 0660,
};

static int __init gh_vm_mgr_init(void)
{
	int ret;

	ret = gh_vcpu_function_init();
	if (ret)
		return ret;

	ret = gh_irqfd_function_init();
	if (ret)
		goto err_vcpu;

	ret = gh_ioeventfd_function_init();
	if (ret)
		goto err_irqfd;

	ret = misc_register(&gh_dev);
	if (ret == -EEXIST) {
		/*
		 * The vendor gh_vm_loader_core.c also registers a miscdevice
		 * literally named "gunyah" for the secure-VM loader UABI
		 * (GH_VM_IOCTL_TYPE is lowercase "g", ours is uppercase "G").
		 * The device config has CONFIG_GH_VM_LOADER unset so that driver
		 * is not built and this cannot happen today, but if a future
		 * config enables it the two collide here and the VM manager
		 * loses. Fail loudly rather than silently having no /dev/gunyah.
		 */
		pr_err("/dev/gunyah already registered by another driver; "
		       "check CONFIG_GH_VM_LOADER\n");
		goto err_ioeventfd;
	}
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		goto err_ioeventfd;
	}

	pr_info("Gunyah VM manager loaded; /dev/gunyah ready\n");
	return 0;

err_ioeventfd:
	gh_ioeventfd_function_exit();
err_irqfd:
	gh_irqfd_function_exit();
err_vcpu:
	gh_vcpu_function_exit();
	return ret;
}

static void __exit gh_vm_mgr_exit(void)
{
	misc_deregister(&gh_dev);
	gh_ioeventfd_function_exit();
	gh_irqfd_function_exit();
	gh_vcpu_function_exit();
	pr_info("Gunyah VM manager unloaded\n");
}

module_init(gh_vm_mgr_init);
module_exit(gh_vm_mgr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah VM manager for Redmi K60 (mondrian)");
