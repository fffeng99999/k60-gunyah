/*
 * RM RPC message IDs that the 5.10 vendor gh_rm_drv does not define but
 * the VM manager needs.
 *
 * Every number here is taken from the upstream Gunyah v17 patch series
 * (tools/src/gunyah-v17-full.mbox, the rsc_mgr_rpc.c hunks), NOT guessed.
 * They are added here rather than editing the vendor header so the patch
 * stays reviewable and revertible.
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

/* VM management, upstream v17 numbering */
#ifndef GH_RM_RPC_MSG_ID_CALL_VM_INIT
#define GH_RM_RPC_MSG_ID_CALL_VM_INIT			0x5600000B
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_CONFIG_IMAGE
#define GH_RM_RPC_MSG_ID_CALL_VM_CONFIG_IMAGE		0x56000009
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_GET_VMID
#define GH_RM_RPC_MSG_ID_CALL_VM_GET_VMID		0x56000024
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT	0x56000031
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_SET_DEMAND_PAGING
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_DEMAND_PAGING	0x56000033
#endif

#ifndef GH_RM_RPC_MSG_ID_CALL_VM_SET_ADDRESS_LAYOUT
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_ADDRESS_LAYOUT	0x56000034
#endif

/* Memory management, upstream v17 numbering */
#ifndef GH_RM_RPC_MSG_ID_CALL_MEM_APPEND
#define GH_RM_RPC_MSG_ID_CALL_MEM_APPEND		0x51000018
#endif

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
 * Copied verbatim from the upstream v17 rsc_mgr_rpc.c hunk, which is
 * little-endian, unlike the native-endian vendor payloads.
 *
 * Note reg_set and reg_index are two separate u8 fields, not one u32:
 * the UABI packs them as (reg << GH_VM_BOOT_CONTEXT_REG_SHIFT) | index,
 * and the driver splits them apart before filling this struct.
 */
struct gh_vm_set_boot_ctx_req {
	__le16 vmid;
	u8 reg_set;
	u8 reg_index;
	__le32 _padding;
	__le64 value;
} __packed;

#endif /* _GH_RM_MSGID_EXT_H */
