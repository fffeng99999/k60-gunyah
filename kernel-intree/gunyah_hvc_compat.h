/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Gunyah hypervisor interface for the K60 in-tree VM manager.
 *
 * The 5.10 vendor tree does NOT have 6.6's linux/gunyah.h, and it does not
 * have arm_smccc_1_2_hvc() either (that arrived in a later kernel). What it
 * does have is arch/arm64/include/asm/gunyah/hcall.h, whose _gh_hcall()
 * passes the function id as the HVC immediate and returns all eight
 * argument registers. That is what this file is built on.
 *
 * Function numbering differs from mainline in the base only:
 *
 *   mainline 6.6        this device (vendor 5.10)
 *   HYP_IDENTIFY  0x8000        0x6000
 *   BELL_SEND     0x8012        0x6012
 *   BELL_SET_MASK 0x8015        0x6015
 *   MSGQ_SEND     0x801B        0x601B
 *   MSGQ_RECV     0x801C        0x601C
 *   VCPU_RUN      0x8065        0x6065
 *
 * The offsets are identical; only the base differs. Verified against
 * drivers/virt/gunyah/hcall_ctrl.h, hcall_dbl.h and hcall_msgq.h in the
 * 5.10.252 tree, and against the working hyp-identify probe that used
 * 0x6000 on this device.
 *
 * Bell send / set-mask are NOT reimplemented here: the vendor tree already
 * provides gh_hcall_dbl_send() and gh_hcall_dbl_mask() in hcall_dbl.h.
 */

#ifndef _GUNYAH_HVC_COMPAT_H
#define _GUNYAH_HVC_COMPAT_H

#include <linux/errno.h>
#include <linux/types.h>

#include <linux/gunyah/gh_common.h>

#include <asm/gunyah/hcall.h>
#include <linux/gunyah/hcall_common.h>

/* Hypercall function ids on this device. */
#define GH_HCALL_HYP_IDENTIFY		0x6000
#define GH_HCALL_DBL_SEND		0x6012
#define GH_HCALL_DBL_SET_MASK		0x6015
#define GH_HCALL_MSGQ_SEND		0x601B
#define GH_HCALL_MSGQ_RECV		0x601C
#define GH_HCALL_VCPU_RUN		0x6065

/*
 * Hypervisor error codes, in the form _gh_hcall() returns them.
 *
 * These match include/linux/gunyah/gh_errno.h, repeated as an enum because
 * the vCPU loop switches on them.
 */
enum gh_error {
	GH_ERROR_OK			= 0,
	GH_ERROR_UNIMPLEMENTED		= -1,
	GH_ERROR_RETRY			= -2,

	GH_ERROR_ARG_INVAL		= 1,
	GH_ERROR_ARG_SIZE		= 2,
	GH_ERROR_ARG_ALIGN		= 3,

	GH_ERROR_NOMEM			= 10,

	GH_ERROR_ADDR_OVFL		= 20,
	GH_ERROR_ADDR_UNFL		= 21,
	GH_ERROR_ADDR_INVAL		= 22,

	GH_ERROR_DENIED			= 30,
	GH_ERROR_BUSY			= 31,
	GH_ERROR_IDLE			= 32,

	GH_ERROR_IRQ_BOUND		= 40,
	GH_ERROR_IRQ_UNBOUND		= 41,

	GH_ERROR_CSPACE_CAP_NULL	= 50,
	GH_ERROR_CSPACE_CAP_REVOKED	= 51,
	GH_ERROR_CSPACE_WRONG_OBJ_TYPE	= 52,
	GH_ERROR_CSPACE_INSUF_RIGHTS	= 53,
	GH_ERROR_CSPACE_FULL		= 54,

	GH_ERROR_MSGQUEUE_EMPTY		= 60,
	GH_ERROR_MSGQUEUE_FULL		= 61,
};

static inline int gh_error_remap(enum gh_error gh_error)
{
	switch (gh_error) {
	case GH_ERROR_OK:
		return 0;
	case GH_ERROR_NOMEM:
		return -ENOMEM;
	case GH_ERROR_DENIED:
	case GH_ERROR_CSPACE_CAP_NULL:
	case GH_ERROR_CSPACE_CAP_REVOKED:
	case GH_ERROR_CSPACE_WRONG_OBJ_TYPE:
	case GH_ERROR_CSPACE_INSUF_RIGHTS:
	case GH_ERROR_CSPACE_FULL:
		return -EACCES;
	case GH_ERROR_BUSY:
	case GH_ERROR_IDLE:
		return -EBUSY;
	case GH_ERROR_IRQ_BOUND:
	case GH_ERROR_IRQ_UNBOUND:
	case GH_ERROR_MSGQUEUE_FULL:
	case GH_ERROR_MSGQUEUE_EMPTY:
		return -EIO;
	case GH_ERROR_UNIMPLEMENTED:
	case GH_ERROR_RETRY:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

/*
 * vCPU run response.
 *
 * The layout matches the 6.6 struct gh_hypercall_vcpu_run_resp, but the
 * registers arrive through _gh_hcall()'s eight-register response rather
 * than arm_smccc_1_2_hvc().
 */
struct gh_hypercall_vcpu_run_resp {
	union {
		enum {
			/* vCPU is ready to run */
			GH_VCPU_STATE_READY		= 0,
			/* vCPU is sleeping until an interrupt arrives */
			GH_VCPU_STATE_EXPECTS_WAKEUP	= 1,
			/* vCPU is powered off */
			GH_VCPU_STATE_POWERED_OFF	= 2,
			/* vCPU is blocked in EL2 for unspecified reason */
			GH_VCPU_STATE_BLOCKED		= 3,
			/* vCPU returned for an MMIO read */
			GH_VCPU_ADDRSPACE_VMMIO_READ	= 4,
			/* vCPU returned for an MMIO write */
			GH_VCPU_ADDRSPACE_VMMIO_WRITE	= 5,
		} state;
		u64 sized_state;
	};
	u64 state_data[3];
};

/*
 * Run a vCPU.
 *
 * x0 = capid, x1..x3 = resume_data, x4 = 0.
 * On return x0 = error, x1 = state, x2..x4 = state_data.
 *
 * Note the 6.6 version passes the function id in x0 because it uses the
 * SMCCC 1.1 HVC #0 convention. Here the id is the HVC immediate, so x0 is
 * the first real argument. The response register layout is the same either
 * way, which is why gh_vcpu_run() does not need to care.
 */
static inline enum gh_error gh_hypercall_vcpu_run(
	u64 capid, u64 *resume_data, struct gh_hypercall_vcpu_run_resp *resp)
{
	struct gh_hcall_resp _resp = { 0 };

	_gh_hcall(GH_HCALL_VCPU_RUN,
		  (struct gh_hcall_args){ capid, resume_data[0],
					  resume_data[1], resume_data[2], 0 },
		  &_resp);

	if (_resp.resp0 == GH_ERROR_OK) {
		resp->sized_state = _resp.resp1;
		resp->state_data[0] = _resp.resp2;
		resp->state_data[1] = _resp.resp3;
		resp->state_data[2] = _resp.resp4;
	}

	return (enum gh_error)_resp.resp0;
}

#endif /* _GUNYAH_HVC_COMPAT_H */
