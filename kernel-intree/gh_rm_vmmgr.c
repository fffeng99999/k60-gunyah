// SPDX-License-Identifier: GPL-2.0-only
/*
 * K60 in-tree Resource Manager extensions for the VM manager.
 *
 * This file is compiled into gh_rm_drv (gh_rm_drv-y in the Makefile) so it
 * can use the file-static gh_rm_call() and gh_rm_notifier directly. Nothing
 * here changes the behaviour of the existing vendor clients.
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

/* Same wire format the 6.6 rsc_mgr_rpc.c uses for VM_ALLOC_VMID. */
struct gh_rm_vm_alloc_req {
	__le16 vmid;
	__le16 reserved;
} __packed;

struct gh_rm_vm_alloc_resp {
	__le16 vmid;
	__le16 reserved;
} __packed;

int gh_rm_vm_alloc_vmid_any(u16 *vmid)
{
	struct gh_rm_vm_alloc_req req = {
		/* 0 = let RM choose, exactly like gh_rm_alloc_vmid(rm, 0) */
		.vmid = 0,
	};
	struct gh_rm_vm_alloc_resp *resp;
	size_t resp_size;
	int rm_err, ret;

	if (!vmid)
		return -EINVAL;

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

	ret = le16_to_cpu(resp->vmid);
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
	struct gh_vm_common_vmid_req req = {
		.vmid = cpu_to_le16(vmid),
	};
	size_t resp_size;
	int rm_err;
	void *resp;

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
	struct gh_vm_common_vmid_req req = {
		.vmid = cpu_to_le16(vmid),
	};
	size_t resp_size;
	int rm_err;
	void *resp;

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
 * Re-export of the file-static gh_rm_call() in gh_rm_core.c.
 *
 * The Android common kernel added RM messages that the vendor driver never
 * learned about, most importantly VM_SET_BOOT_CONTEXT. Sending them
 * directly gives a clean rm_err when RM does not support them, instead of
 * a silent hang.
 */
void *gh_rm_call_raw(u32 message_id, const void *req_buf,
		     size_t req_buf_size, size_t *resp_buff_size,
		     int *rm_error)
{
	return gh_rm_call(message_id, req_buf, req_buf_size, resp_buff_size,
			  rm_error);
}
EXPORT_SYMBOL_GPL(gh_rm_call_raw);
