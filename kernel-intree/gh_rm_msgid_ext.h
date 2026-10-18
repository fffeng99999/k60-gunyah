/*
 * RM RPC message IDs that the 5.10 vendor gh_rm_drv does not define but
 * the VM manager needs.
 *
 * These are taken from the upstream 6.6 rsc_mgr_rpc.c and the Android
 * common kernel. They are added here rather than editing the vendor header
 * so the patch stays reviewable and revertible.
 *
 * IMPORTANT: sending one of these to an RM that does not implement it
 * returns a normal rm_err, not a hang. The VM manager logs the rejection
 * and continues, so a missing capability degrades instead of hanging.
 */
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GH_RM_MSGID_EXT_H
#define _GH_RM_MSGID_EXT_H

#include <linux/types.h>

#include <linux/gunyah/gh_common.h>

/* VM management, upstream 6.6 numbering */
#ifndef GH_RM_RPC_MSG_ID_CALL_VM_INIT
#define GH_RM_RPC_MSG_ID_CALL_VM_INIT			0x5600000B
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_CONFIG_IMAGE
#define GH_RM_RPC_MSG_ID_CALL_VM_CONFIG_IMAGE		0x56000009
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_GET_VMID
#define GH_RM_RPC_MSG_ID_CALL_VM_GET_VMID		0x56000024
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_SET_FIRMWARE_MEM
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_FIRMWARE_MEM	0x56000032
#endif

/*
 * VM_SET_BOOT_CONTEXT is an Android common-kernel addition and is not in
 * upstream 6.6 at all. It carries a register id and a value, and is the
 * only way to set the guest entry point on an RM without VM_CONFIG_IMAGE.
 *
 * The number is the value the DroidVM crosvm binary was built against; it
 * is a vendor/Android-private namespace and is not stable across kernels,
 * so verify it against the running crosvm if the ioctl ever returns ENOTTY.
 */
#ifndef GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT	0x56000040
#endif

/* Memory management, upstream 6.6 numbering */
#ifndef GH_RM_RPC_MSG_ID_CALL_MEM_APPEND
#define GH_RM_RPC_MSG_ID_CALL_MEM_APPEND		0x51000018
#endif

/*
 * struct gh_vm_common_vmid_req - the "just a vmid please" request body used
 * by VM_START / VM_RESET / VM_INIT / VM_DEALLOCATE.
 *
 * The vendor private header has this inline in each payload struct, but the
 * new VM_INIT / VM_SET_BOOT_CONTEXT helpers need it as a named type.
 */
struct gh_vm_common_vmid_req {
	__le16 vmid;
	__le16 reserved;
} __packed;

/*
 * struct gh_vm_init_req_payload - VM_INIT request body.
 *
 * Not in the vendor tree because the vendor never calls VM_INIT. Same
 * shape as VM_START / VM_DEALLOCATE / VM_RESET: a vmid plus padding.
 * Native-endian, matching every other vendor VM payload.
 */
struct gh_vm_init_req_payload {
	gh_vmid_t vmid;
	u16 reserved;
} __packed;

/*
 * struct gh_vm_set_boot_ctx_req - VM_SET_BOOT_CONTEXT request body.
 *
 * An Android common-kernel addition with no vendor or upstream equivalent,
 * so the layout below is what the DroidVM crosvm binary was built against.
 * If the ioctl is ever rejected with rm_err this is the first thing to
 * re-check.
 */
struct gh_vm_set_boot_ctx_req {
	gh_vmid_t vmid;
	u16 reserved;
	u32 reg;
	u64 value;
} __packed;

#endif /* _GH_RM_MSGID_EXT_H */
