/*
 * K60 in-tree VM manager compatibility shims.
 *
 * Collects the handful of things the 5.10 vendor Gunyah tree does not
 * provide but the 6.6-derived VM manager code expects:
 *
 *   - struct gh_rm_mem_parcel and its entry types
 *   - the VM stop reason constants
 *   - GH_MEM_HANDLE_INVAL
 *   - gh_rm_vm_auth_mechanism
 *   - gh_remap_error as a plain function
 *
 * Everything here is guarded so a future vendor update that adds the real
 * definitions silently wins.
 */
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _GH_VM_MGR_COMPAT_H
#define _GH_VM_MGR_COMPAT_H

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <linux/gunyah/gh_common.h>
#include <linux/gunyah/gh_errno.h>

#ifndef GH_MEM_HANDLE_INVAL
#define GH_MEM_HANDLE_INVAL	U32_MAX
#endif

/*
 * VM stop reasons. gh_rm_vm_stop() takes a u32 stop_reason; the vendor
 * header only defines the force-stop flag, not the reason value.
 */
#ifndef GH_VM_STOP_REASON_FORCE_STOP
#define GH_VM_STOP_REASON_FORCE_STOP		3
#endif

/* Authentication mechanisms for gh_rm_vm_configure() and the UABI. */
#ifndef gh_rm_vm_auth_mechanism
enum gh_rm_vm_auth_mechanism {
	GH_RM_VM_AUTH_NONE		= 0,
	GH_RM_VM_AUTH_QCOM_PIL_ELF	= 1,
	GH_RM_VM_AUTH_QCOM_ANDROID_PVM	= 2,
};
#endif

/*
 * Memory parcel description.
 *
 * The vendor gh_rm_iface.c takes three separate descriptor structs
 * (gh_acl_desc, gh_sgl_desc, gh_mem_attr_desc) and frames the RPC itself.
 * The VM manager keeps a combined view so a region can be described once
 * and handed over later, which is what the 6.6 vm_mgr_mm.c does.
 *
 * The field names deliberately match the vendor descriptor members so the
 * copy in gh_vm_mem_parcel_out() is a plain struct assignment.
 */
#ifndef gh_rm_mem_parcel
struct gh_rm_mem_acl_entry {
	gh_vmid_t vmid;
	u8 perms;
	u8 reserved;
} __packed;

struct gh_rm_mem_entry {
	u64 ipa_base;
	u64 size;
} __packed;

struct gh_rm_mem_attr_entry {
	u16 attr;
	u16 vmid;
} __packed;

/*
 * A combined view of one parcel. The three arrays are handed straight to
 * the vendor gh_rm_mem_share()/gh_rm_mem_lend() descriptors, so the member
 * names match those descriptors on purpose.
 */
struct gh_rm_mem_parcel {
	u8 mem_type;
	gh_label_t label;
	size_t n_acl_entries;
	struct gh_rm_mem_acl_entry *acl_entries;
	size_t n_sgl_entries;
	struct gh_rm_mem_entry *sgl_entries;
	size_t n_mem_attr_entries;
	struct gh_rm_mem_attr_entry *mem_attr_entries;
	gh_memparcel_handle_t mem_handle;
};
#endif

/*
 * gh_remap_error() exists in gh_errno.h as a static inline that maps to
 * slightly different errnos than the 6.6 version (notably IRQ errors map
 * to -EPERM instead of -EIO). The VM manager reports these codes to
 * userspace, and crosvm's diagnostics key off the standard mapping, so use
 * the 6.6 semantics.
 */
#endif /* _GH_VM_MGR_COMPAT_H */
