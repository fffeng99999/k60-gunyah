/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal header for the K60 in-tree Gunyah VM manager.
 */

#ifndef _GH_VM_MGR_H
#define _GH_VM_MGR_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/rb_node.h>
#include <linux/rwsem.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <linux/gunyah/gh_common.h>

#include "gh_vm_mgr_compat.h"

#include <uapi/linux/gunyah.h>

#define GH_VM_MAX_BOOT_CTX	8

/* RM resource types, as returned by VM_GET_HYP_RESOURCES. */
enum gh_rm_res_type {
	GH_RM_RES_TYPE_DB_TX		= 0,
	GH_RM_RES_TYPE_DB_RX		= 1,
	GH_RM_RES_TYPE_MQ_TX		= 2,
	GH_RM_RES_TYPE_MQ_RX		= 3,
	GH_RM_RES_TYPE_VCPU		= 4,
	GH_RM_RES_TYPE_VPMGRP		= 5,
	GH_RM_RES_TYPE_VIRTIO_MMIO	= 6,
};

/*
 * Wire layout of one VM_GET_HYP_RESOURCES entry on this kernel.
 *
 * This is NOT the upstream v17 struct gunyah_rm_hyp_resource: the
 * vendor payload splits the 64-bit fields into low/high halves, and
 * partner_vmid is native-endian rather than __le16.
 */
struct gh_vmmgr_hyp_res_entry {
	u8 res_type;
	u8 reserved;
	gh_vmid_t partner_vmid;
	__le32 resource_handle;
	__le32 resource_label;
	__le32 cap_id_low;
	__le32 cap_id_high;
	__le32 virq_handle;
	__le32 virq;
	__le32 base_low;
	__le32 base_high;
	__le32 size_low;
	__le32 size_high;
} __packed;

struct gh_vmmgr_hyp_res_payload {
	__le32 n_resource_entries;
	struct gh_vmmgr_hyp_res_entry entries[];
} __packed;


struct gh_resource {
	enum gh_rm_res_type type;
	u32 rm_label;
	u64 capid;
	u32 virq_handle;
	s32 virq;
	int irq;
	u64 base;
	u64 size;

	struct list_head list;
};

struct gh_vm_mem {
	struct list_head list;
	enum gh_vm_mem_share_type share_type;
	struct gh_rm_mem_parcel parcel;

	__u64 guest_phys_addr;
	struct page **pages;
	unsigned long npages;
};

struct gh_vm_function_instance;

struct gh_vm_function {
	u32 type;
	const char *name;
	struct module *mod;
	long (*bind)(struct gh_vm_function_instance *f);
	void (*unbind)(struct gh_vm_function_instance *f);
	bool (*compare)(const struct gh_vm_function_instance *f,
			const void *arg, size_t size);
};

struct gh_vm_function_instance {
	size_t arg_size;
	void *argp;
	struct gh_vm *ghvm;
	struct gh_vm_function *fn;
	void *data;
	struct list_head vm_list;
};

struct gh_vm_resource_ticket {
	struct list_head vm_list;
	struct list_head resources;
	enum gh_rm_res_type resource_type;
	u32 label;

	struct module *owner;
	bool (*populate)(struct gh_vm_resource_ticket *ticket,
			 struct gh_resource *ghrsc);
	void (*unpopulate)(struct gh_vm_resource_ticket *ticket,
			   struct gh_resource *ghrsc);
};

struct gh_vm_io_handler_ops;

struct gh_vm_io_handler {
	struct rb_node node;
	u64 addr;

	bool datamatch;
	u8 len;
	u64 data;
	struct gh_vm_io_handler_ops *ops;
};

struct gh_vm_io_handler_ops {
	int (*read)(struct gh_vm_io_handler *io_dev, u64 addr, u32 len,
		    u64 data);
	int (*write)(struct gh_vm_io_handler *io_dev, u64 addr, u32 len,
		     u64 data);
};

struct gh_vm_boot_ctx_entry {
	u32 reg;
	u64 value;
};

struct gh_vm {
	u16 vmid;
	/*
	 * Kept for parity with upstream. The miscdevice holds the only
	 * struct device this driver owns, so per-VM devices are not
	 * allocated; every pr_* here uses the module prefix instead.
	 */
	struct device *parent;
	enum gh_rm_vm_auth_mechanism auth;
	struct gh_vm_dtb_config dtb_config;
	struct gh_vm_firmware_config fw_config;

	struct notifier_block nb;
	enum gh_rm_vm_status vm_status;
	wait_queue_head_t vm_status_wait;
	struct rw_semaphore status_lock;
	struct gh_vm_exit_info exit_info;

	struct work_struct free_work;
	struct kref kref;
	struct mm_struct *mm;
	struct mutex mm_lock;
	struct list_head memory_mappings;
	struct mutex fn_lock;
	struct list_head functions;
	struct mutex resources_lock;
	struct list_head resources;
	struct list_head resource_tickets;
	struct rb_root mmio_handler_root;
	struct rw_semaphore mmio_handler_lock;

	struct gh_vm_boot_ctx_entry boot_ctx[GH_VM_MAX_BOOT_CTX];
	u32 n_boot_ctx;
};

/* vm_mgr.c */
int gh_vm_mem_alloc(struct gh_vm *ghvm,
		    struct gh_userspace_memory_region *region, bool lend);
void gh_vm_mem_reclaim(struct gh_vm *ghvm);
struct gh_vm_mem *gh_vm_mem_find_by_addr(struct gh_vm *ghvm,
					 u64 guest_phys_addr, u32 size);
int gh_vm_mmio_write(struct gh_vm *ghvm, u64 addr, u32 len, u64 data);

int gh_vm_get(struct gh_vm *ghvm);
void gh_vm_put(struct gh_vm *ghvm);

int gh_vm_add_resource_ticket(struct gh_vm *ghvm,
			      struct gh_vm_resource_ticket *ticket);
void gh_vm_remove_resource_ticket(struct gh_vm *ghvm,
				  struct gh_vm_resource_ticket *ticket);

int gh_vm_add_io_handler(struct gh_vm *ghvm, struct gh_vm_io_handler *io_dev);
void gh_vm_remove_io_handler(struct gh_vm *ghvm,
			     struct gh_vm_io_handler *io_dev);

/* gunyah_vcpu.c */
int gh_vcpu_function_init(void);
void gh_vcpu_function_exit(void);

/* gunyah_irqfd.c */
int gh_irqfd_function_init(void);
void gh_irqfd_function_exit(void);

/* gunyah_ioeventfd.c */
int gh_ioeventfd_function_init(void);
void gh_ioeventfd_function_exit(void);

#endif /* _GH_VM_MGR_H */
