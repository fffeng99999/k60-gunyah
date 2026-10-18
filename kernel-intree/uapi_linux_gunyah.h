/*
 * Userspace ABI for /dev/gunyah - Gunyah based virtual machine.
 *
 * This header is a merge of three sources and must stay byte-compatible
 * with the crosvm "droidvm" branch, which is what DroidVM ships:
 *
 *   1. upstream 6.6 include/uapi/linux/gunyah.h  (base ioctls)
 *   2. Android common-kernel extensions             (GH_ANDROID_IOCTL_TYPE)
 *   3. DroidVM crosvm additions                     (boot context, page fault)
 *
 * Numbering is fixed by the shipped crosvm binary and MUST NOT be changed:
 *
 *   GH_IOCTL_TYPE       'G' (0x47)
 *   GH_ANDROID_IOCTL_TYPE 'A' (0x41)
 */
/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_GUNYAH_H
#define _UAPI_LINUX_GUNYAH_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define GH_IOCTL_TYPE			'G'

/* ioctls for /dev/gunyah fds */
#define GH_CREATE_VM			_IO(GH_IOCTL_TYPE, 0x0)

enum gh_mem_flags {
	GH_MEM_ALLOW_READ	= 1UL << 0,
	GH_MEM_ALLOW_WRITE	= 1UL << 1,
	GH_MEM_ALLOW_EXEC	= 1UL << 2,
};

struct gh_userspace_memory_region {
	__u32 label;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size;
	__u64 userspace_addr;
};

#define GH_VM_SET_USER_MEM_REGION	_IOW(GH_IOCTL_TYPE, 0x1, \
						struct gh_userspace_memory_region)

struct gh_vm_dtb_config {
	__u64 guest_phys_addr;
	__u64 size;
};
#define GH_VM_SET_DTB_CONFIG		_IOW(GH_IOCTL_TYPE, 0x2, struct gh_vm_dtb_config)

#define GH_VM_START			_IO(GH_IOCTL_TYPE, 0x3)

enum gh_fn_type {
	GH_FN_VCPU = 1,
	GH_FN_IRQFD,
	GH_FN_IOEVENTFD,
};

#define GH_FN_MAX_ARG_SIZE		256

struct gh_fn_vcpu_arg {
	__u32 id;
};

enum gh_irqfd_flags {
	GH_IRQFD_FLAGS_LEVEL		= 1UL << 0,
};

struct gh_fn_irqfd_arg {
	__u32 fd;
	__u32 label;
	__u32 flags;
	__u32 padding;
};

enum gh_ioeventfd_flags {
	GH_IOEVENTFD_FLAGS_DATAMATCH	= 1UL << 0,
};

struct gh_fn_ioeventfd_arg {
	__u64 datamatch;
	__u64 addr;
	__u32 len;
	__s32 fd;
	__u32 flags;
	__u32 padding;
};

struct gh_fn_desc {
	__u32 type;
	__u32 arg_size;
	__u64 arg;
};

#define GH_VM_ADD_FUNCTION		_IOW(GH_IOCTL_TYPE, 0x4, struct gh_fn_desc)
#define GH_VM_REMOVE_FUNCTION		_IOW(GH_IOCTL_TYPE, 0x7, struct gh_fn_desc)

enum gh_vm_exit_status {
	GH_VM_STATUS_LOAD_FAILED	= 1,
	GH_VM_STATUS_EXITED		= 2,
	GH_VM_STATUS_CRASHED		= 3,
};

#define GH_VM_MAX_EXIT_REASON_SIZE	8u

struct gh_vm_exit_info {
	__u16 type;
	__u16 padding;
	__u32 reason_size;
	__u8 reason[GH_VM_MAX_EXIT_REASON_SIZE];
};

enum gh_vcpu_exit {
	GH_VCPU_EXIT_UNKNOWN,
	GH_VCPU_EXIT_MMIO,
	GH_VCPU_EXIT_STATUS,
	GH_VCPU_EXIT_PAGE_FAULT,	/* DroidVM crosvm addition */
};

struct gh_vcpu_run {
	/* in */
	__u8 immediate_exit;
	__u8 padding[7];

	/* out */
	__u32 exit_reason;

	union {
		struct {
			__u64 phys_addr;
			__u8  data[8];
			__u32 len;
			__u8  is_write;
		} mmio;

		struct {
			enum gh_vm_exit_status status;
			struct gh_vm_exit_info exit_info;
		} status;

		/* DroidVM crosvm addition: stage-2 fault reporting. */
		struct {
			__u64 phys_addr;
			__s32 attempt;
			__u8  resume_action;
		} page_fault;
	};
};

#define GH_VCPU_RUN			_IO(GH_IOCTL_TYPE, 0x5)
#define GH_VCPU_MMAP_SIZE		_IO(GH_IOCTL_TYPE, 0x6)

/*
 * Boot context (DroidVM crosvm / Android common kernel).
 *
 * crosvm calls GH_VM_SET_BOOT_CONTEXT to place the guest entry point in a
 * register before VM_START. The device 5.10 RM has no VM_CONFIG_IMAGE, so
 * this is the only way to tell the hypervisor where to begin execution.
 */
#define GH_VM_BOOT_CONTEXT_REG_SHIFT	8

enum gh_vm_boot_context_reg {
	GH_VM_BOOT_REG_SET_X	= 0,
	GH_VM_BOOT_REG_SET_PC	= 1,
	GH_VM_BOOT_REG_SET_SP	= 2,
};

struct gh_vm_boot_context {
	__u32 reg;
	__u32 reserved;
	__u64 value;
};

#define GH_VM_SET_BOOT_CONTEXT		_IOW(GH_IOCTL_TYPE, 0xa, struct gh_vm_boot_context)

/*
 * ANDROID: android14-6.1 unfortunately contains UAPI that won't be carried
 * in kernel.org. Expose orthogonal ioctls that will never conflict with
 * kernel.org for these UAPIs. See b/268234781.
 */
#define GH_ANDROID_IOCTL_TYPE		'A'

#define GH_VM_ANDROID_LEND_USER_MEM	_IOW(GH_ANDROID_IOCTL_TYPE, 0x11, \
						struct gh_userspace_memory_region)

struct gh_vm_firmware_config {
	__u64 guest_phys_addr;
	__u64 size;
};

#define GH_VM_ANDROID_SET_FW_CONFIG	_IOW(GH_ANDROID_IOCTL_TYPE, 0x12, \
						struct gh_vm_firmware_config)

struct gunyah_address_range {
	__u64 guest_phys_addr;
	__u64 size;
};

#define GH_VM_RECLAIM_REGION		_IOW(GH_ANDROID_IOCTL_TYPE, 0x13, \
						struct gunyah_address_range)


struct gh_vm_share_blob {
	__u32 label;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size;
	__u64 userspace_addr;
	__u32 mem_handle;
	__u32 padding;
};

#define GH_VM_ANDROID_SHARE_BLOB	_IOWR(GH_ANDROID_IOCTL_TYPE, 0x14, \
						struct gh_vm_share_blob)

enum gunyah_map_flags {
	GUNYAH_MEM_ALLOW_READ		= 1,
	GUNYAH_MEM_ALLOW_WRITE		= 2,
	GUNYAH_MEM_ALLOW_EXEC		= 4,
	GUNYAH_MEM_ALLOW_RWX		= 7,
	GUNYAH_MEM_DEFAULT_ACCESS	= 0,
	GUNYAH_MEM_FORCE_LEND		= 16,
	GUNYAH_MEM_FORCE_SHARE		= 32,
	GUNYAH_MEM_UNMAP		= 256,
};

struct gunyah_map_cma_mem_args {
	__u32 label;
	__u64 guest_addr;
	__u32 flags;
	__u32 guest_mem_fd;
	__u64 offset;
	__u64 size;
};

#define GH_VM_ANDROID_MAP_CMA_MEM	_IOW(GH_ANDROID_IOCTL_TYPE, 0x15, \
						struct gunyah_map_cma_mem_args)

enum gunyah_auth_type {
	GUNYAH_ANDROID_PVM_TYPE		= 0,
	GUNYAH_QCOM_TRUSTED_VM_TYPE	= 1,
};

struct gunyah_qtvm_auth_arg {
	__u16 vm_id;
	__u32 pas_id;
	__u64 guest_phys_addr;
	__u64 size;
};

struct gunyah_auth_desc {
	__u32 type_;
	__u32 arg_size;
	__u64 arg;
};

#define GH_VM_ANDROID_SET_AUTH_TYPE	_IOW(GH_ANDROID_IOCTL_TYPE, 0x16, \
						struct gunyah_auth_desc)

#endif /* _UAPI_LINUX_GUNYAH_H */
