// SPDX-License-Identifier: GPL-2.0-only
/*
 * K60 in-tree Resource Manager extensions for the VM manager.
 *
 * This file is compiled into gh_rm_drv (gh_rm_drv-y in the Makefile) so it
 * can use the module-global gh_rm_call() and the RM notifier chain
 * directly. Nothing here changes the behaviour of the existing vendor
 * clients, and gh_rm_vmmgr.o has no module_init, so adding it does not
 * change when or whether gh_rm_drv loads.
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

#define pr_fmt(fmt) "gh_rm_vmmgr: " fmt

#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/notifier.h>
#include <linux/string.h>

#include <linux/gunyah/gh_common.h>
#include <linux/gunyah/gh_errno.h>
#include <linux/gunyah/gh_rm_drv.h>

#include "gh_rm_drv_private.h"
#include "gh_rm_vmmgr.h"
#include "gh_rm_msgid_ext.h"

#include <uapi/linux/gunyah.h>	/* GH_VM_BOOT_CONTEXT_REG_SHIFT */

/*
 * VM_ALLOCATE and VM_DEALLOCATE use the vendor payload structs, which are
 * native-endian, not little-endian like the 6.6 wire format. The structs
 * come from gh_rm_drv_private.h and must not be redefined here.
 */
int gh_rm_vm_alloc_vmid_any(u16 *vmid)
{
	struct gh_vm_allocate_req_payload req = {0};
	struct gh_vm_allocate_resp_payload *resp;
	size_t resp_size;
	int rm_err, ret;

	if (!vmid)
		return -EINVAL;

	/*
	 * vmid 0 asks RM to pick one. The vendor wrapper always passes the
	 * value read from DT, which for a dynamic VM is 0.
	 */
	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_ALLOCATE,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_ALLOCATE rejected by RM: rm_err=%d\n", rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_ALLOCATE failed: %ld\n", PTR_ERR(resp));
		return PTR_ERR(resp);
	}
	if (resp_size != sizeof(*resp)) {
		pr_err("VM_ALLOCATE bad response size %zu\n", resp_size);
		kfree(resp);
		return -EBADMSG;
	}

	ret = resp->vmid;
	kfree(resp);

	if (ret <= 0) {
		pr_err("VM_ALLOCATE returned invalid vmid %d\n", ret);
		return -EINVAL;
	}

	*vmid = ret;
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_alloc_vmid_any);

int gh_rm_vm_dealloc_vmid_any(u16 vmid)
{
	struct gh_vm_deallocate_req_payload req = {
		.vmid = vmid,
	};
	size_t resp_size;
	int rm_err;
	void *resp;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_DEALLOCATE,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_DEALLOCATE(%u) rejected by RM: rm_err=%d\n",
		       vmid, rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_DEALLOCATE(%u) failed: %ld\n", vmid, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	kfree(resp);
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_dealloc_vmid_any);

struct gh_rm_reset_waiter {
	struct notifier_block nb;
	struct completion done;
	u16 vmid;
	bool reset_seen;
};

static int gh_rm_reset_status_cb(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct gh_rm_reset_waiter *w =
		container_of(nb, struct gh_rm_reset_waiter, nb);
	struct gh_rm_notif_vm_status_payload *payload = data;

	if (action != GH_RM_NOTIF_VM_STATUS)
		return NOTIFY_OK;

	if (payload->vmid != w->vmid)
		return NOTIFY_OK;

	if (payload->vm_status == GH_RM_VM_STATUS_RESET) {
		w->reset_seen = true;
		complete(&w->done);
		return NOTIFY_OK;
	}

	/*
	 * RM also reports a reset when the VM dies for any reason; that still
	 * means the VMID is no longer in use, so let the waiter out.
	 */
	if (payload->vm_status == GH_RM_VM_STATUS_EXITED) {
		w->reset_seen = true;
		complete(&w->done);
		return NOTIFY_OK;
	}

	return NOTIFY_OK;
}

int gh_rm_vm_reset_sync(u16 vmid)
{
	struct gh_rm_reset_waiter w = {
		.nb = { .notifier_call = gh_rm_reset_status_cb },
		.vmid = vmid,
	};
	int ret;

	init_completion(&w.done);

	ret = gh_rm_register_notifier(&w.nb);
	if (ret)
		return ret;

	ret = gh_rm_vm_reset(vmid);
	if (ret) {
		gh_rm_unregister_notifier(&w.nb);
		return ret;
	}

	/*
	 * RM confirms asynchronously. If the VM was never started there is no
	 * status notification coming, so do not wait forever.
	 */
	if (!wait_for_completion_timeout(&w.done,
					 msecs_to_jiffies(5000))) {
		pr_warn("VM_RESET(%u): no RESET notification within 5s\n", vmid);
		ret = -ETIMEDOUT;
	} else if (!w.reset_seen) {
		ret = -EPROTO;
	}

	gh_rm_unregister_notifier(&w.nb);
	return ret;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_reset_sync);

int gh_rm_vm_get_hyp_resources_raw(u16 vmid, void **resources, size_t *size)
{
	struct gh_vm_get_hyp_res_req_payload req = {
		.vmid = vmid,
	};
	struct gh_vm_get_hyp_res_resp_payload *resp;
	size_t resp_size;
	int rm_err;

	if (!resources || !size)
		return -EINVAL;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_GET_HYP_RESOURCES,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_GET_HYP_RESOURCES(%u) rejected: rm_err=%d\n",
		       vmid, rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_GET_HYP_RESOURCES(%u) failed: %ld\n",
		       vmid, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	*resources = resp;
	*size = resp_size;
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_get_hyp_resources_raw);

int gh_rm_vm_bind_virq(u32 virq_handle, s32 virq)
{
	struct gh_vm_irq_accept_req_payload req = {
		.virq_handle = virq_handle,
		.virq = virq,
	};
	struct gh_vm_irq_accept_resp_payload *resp;
	size_t resp_size;
	int rm_err, ret;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_IRQ_ACCEPT,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_IRQ_ACCEPT rejected: rm_err=%d\n", rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_IRQ_ACCEPT failed: %ld\n", PTR_ERR(resp));
		return PTR_ERR(resp);
	}
	if (resp_size != sizeof(*resp)) {
		kfree(resp);
		return -EBADMSG;
	}

	ret = resp->virq;
	kfree(resp);

	if (ret < 0)
		return -EINVAL;

	return ret;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_bind_virq);

void gh_rm_vm_unbind_virq(u32 virq_handle, int linux_irq)
{
	struct gh_vm_irq_reclaim_req_payload req = {
		.virq_handle = virq_handle,
	};
	size_t resp_size;
	int rm_err;
	void *resp;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_IRQ_RECLAIM,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err || IS_ERR_OR_NULL(resp)) {
		pr_warn("VM_IRQ_RECLAIM(%u) failed: rm_err=%d ret=%ld\n",
			virq_handle, rm_err, PTR_ERR(resp));
	} else {
		kfree(resp);
	}

	gh_put_irq(linux_irq);
}
EXPORT_SYMBOL_GPL(gh_rm_vm_unbind_virq);

/* ------------------------------------------------------------------ */
/* VM lifecycle helpers not exported by the vendor gh_rm_iface.c       */
/* ------------------------------------------------------------------ */

int gh_rm_vm_init(u16 vmid)
{
	struct gh_vm_init_req_payload req = {0};

	size_t resp_size;
	int rm_err;
	void *resp;

	req.vmid = vmid;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_INIT,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_INIT(%u) rejected by RM: rm_err=%d\n", vmid, rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_INIT(%u) failed: %ld\n", vmid, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	kfree(resp);
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_init);

int gh_rm_vm_start_raw(u16 vmid)
{
	struct gh_vm_start_req_payload req = {0};

	size_t resp_size;
	int rm_err;
	void *resp;

	req.vmid = vmid;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_START,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_START(%u) rejected by RM: rm_err=%d\n", vmid, rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_START(%u) failed: %ld\n", vmid, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	kfree(resp);
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_start_raw);

int gh_rm_vm_stop_raw(u16 vmid, u32 stop_reason, u8 flags)
{
	struct gh_vm_stop_req_payload req = {
		.vmid = vmid,
		.flags = flags,
		.reserved = 0,
		.stop_reason = stop_reason,
	};
	size_t resp_size;
	int rm_err;
	void *resp;

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_STOP,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_err("VM_STOP(%u) rejected by RM: rm_err=%d\n", vmid, rm_err);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_err("VM_STOP(%u) failed: %ld\n", vmid, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	kfree(resp);
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_vm_stop_raw);

/*
 * The only raw RPC the VM manager needs, exported deliberately.
 *
 * GH_VM_MGR depends on GH_RM_DRV, and the device config sets
 * CONFIG_GH_RM_DRV=m, so GH_VM_MGR can only be n or m and the VM manager
 * has to be its own module. That module cannot see the module-global
 * gh_rm_call() in gh_rm_core.c, hence this single narrow export rather
 * than exporting the generic entry point.
 *
 * VM_SET_BOOT_CONTEXT is absent from the vendor message table. It is
 * little-endian, unlike the vendor payloads, and its register id is two
 * u8 fields: the UABI packs them as
 * (reg_set << GH_VM_BOOT_CONTEXT_REG_SHIFT) | reg_index.
 */
int gh_rm_set_vm_boot_context(u16 vmid, u32 reg, u64 value)
{
	struct gh_vm_set_boot_ctx_req req = {0};
	size_t resp_size;
	int rm_err;
	void *resp;

	req.vmid = cpu_to_le16(vmid);
	req.reg_set = reg >> GH_VM_BOOT_CONTEXT_REG_SHIFT;
	req.reg_index = reg & 0xff;
	req.value = cpu_to_le64(value);

	resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT,
			  &req, sizeof(req), &resp_size, &rm_err);
	if (rm_err) {
		pr_warn("VM_SET_BOOT_CONTEXT(vmid=%u reg=%#x) rejected by RM: rm_err=%d\n",
			vmid, reg, rm_err);
		kfree(resp);
		return gh_remap_error(rm_err);
	}
	if (IS_ERR_OR_NULL(resp)) {
		pr_warn("VM_SET_BOOT_CONTEXT(vmid=%u reg=%#x) failed: %ld\n",
			vmid, reg, PTR_ERR(resp));
		return PTR_ERR(resp);
	}

	kfree(resp);
	return 0;
}
EXPORT_SYMBOL_GPL(gh_rm_set_vm_boot_context);
