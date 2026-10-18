/*
 * K60 in-tree additions to the Gunyah Resource Manager interface.
 *
 * The vendor gh_rm_iface.c keeps a fixed enum gh_vm_names -> vmid table
 * (gh_vm_table) and refuses a second allocation for the same name with
 * -EINVAL, and gh_reset_vm_prop_table_entry() cannot clear an entry that
 * RM never released. That makes it impossible to create more than one
 * non-trusted guest VM from a tree-out-of-tree module.
 *
 * These functions talk to RM directly and deliberately bypass gh_vm_table.
 * They are the kernel-side counterpart of what a userspace VMM needs.
 */
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GH_RM_VMMGR_H
#define _GH_RM_VMMGR_H

#include <linux/types.h>

struct gh_rm;

/*
 * Allocate a fresh VMID without touching the vendor VM-name table.
 *
 * Returns the allocated VMID (>= 1) or a negative error code.
 * On failure the VMID is not consumed and can be retried.
 */
int gh_rm_vm_alloc_vmid_any(u16 *vmid);

/*
 * Release a VMID allocated by gh_rm_vm_alloc_vmid_any().
 *
 * Unlike gh_rm_vm_dealloc_vmid() this does not walk gh_vm_table, so it
 * works for VMIDs the vendor table never knew about.
 */
int gh_rm_vm_dealloc_vmid_any(u16 vmid);

/*
 * Ask RM to reset a VM and wait for the RESET status notification.
 *
 * gh_rm_vm_reset() only sends the request; RM answers asynchronously with
 * GH_RM_NOTIF_VM_STATUS. Without waiting, an immediately following
 * deallocate races the reset and RM answers EPERM.
 */
int gh_rm_vm_reset_sync(u16 vmid);

/*
 * VM lifecycle steps that the vendor gh_rm_iface.c does not export in a
 * usable form. gh_rm_vm_start()/gh_rm_vm_reset() take a bare vmid, but
 * VM_INIT has no exported wrapper at all, so add one.
 */
int gh_rm_vm_init(u16 vmid);
int gh_rm_vm_start_raw(u16 vmid);
int gh_rm_vm_stop_raw(u16 vmid, u32 stop_reason, u8 flags);

/*
 * Send an arbitrary RM RPC and wait for the reply.
 *
 * This is the file-static gh_rm_call() re-exported for the VM manager.
 * Needed because VM_SET_BOOT_CONTEXT and a few other Android extensions
 * are absent from the vendor message table.
 */
void *gh_rm_call_raw(u32 message_id, const void *req_buf,
		     size_t req_buf_size, size_t *resp_buff_size,
		     int *rm_error);

/*
 * Fetch the hypervisor resources (vCPU cap ids, doorbells, msgqueues) that
 * RM assigned to a VM after gh_rm_vm_init().
 */
int gh_rm_vm_get_hyp_resources_raw(u16 vmid, void **resources, size_t *size);

/*
 * Convert a Gunyah vIRQ into a Linux IRQ and bind it, the way
 * gh_rm_populate_hyp_res() does for the vendor VMs.
 */
int gh_rm_vm_bind_virq(u32 virq_handle, s32 virq);

void gh_rm_vm_unbind_virq(u32 virq_handle, int linux_irq);

#endif /* _GH_RM_VMMGR_H */
