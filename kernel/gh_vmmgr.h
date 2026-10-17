/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared definitions for the additive K60 Gunyah VM manager.
 *
 * These ioctl numbers are deliberately NOT the crosvm ABI. The goal of
 * this stage is to expose /dev/gunyah and prove RM allocation behaviour
 * before implementing the full crosvm VM lifecycle ABI.
 */

#ifndef _GH_VMMGR_H
#define _GH_VMMGR_H

#include <linux/types.h>

#define GH_VMMGR_DEVICE_NAME "gunyah"
#define GH_VMMGR_CLASS_NAME "gunyah"

#define GH_VMMGR_IOC_MAGIC 'g'

struct gh_vmmgr_vminfo {
	__u16 primary_vmid;
	__u16 allocated_vmid;
	__u8 has_uri;
	__u8 has_name;
	__u8 reserved[2];
};

#define GH_VMMGR_IOC_QUERY_VMINFO \
	_IOR(GH_VMMGR_IOC_MAGIC, 0x0, struct gh_vmmgr_vminfo)
#define GH_VMMGR_IOC_ALLOC_VMID _IO(GH_VMMGR_IOC_MAGIC, 0x1)
#define GH_VMMGR_IOC_RELEASE_VMID _IO(GH_VMMGR_IOC_MAGIC, 0x2)

#endif /* _GH_VMMGR_H */
