/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Gunyah hypervisor interface for the K60 in-tree VM manager.
 *
 * The 5.10 vendor tree ships hcall_ctrl.h / hcall_dbl.h / hcall_msgq.h /
 * hcall_virtio.h but NOT the 6.6 linux/gunyah.h that declares the vCPU run
 * hypercall, the bell send/set-mask hypercalls, or the hyp-identify
 * wrapper. The VM manager and its vCPU function need all of those.
 *
 * Only the pieces actually used are declared, with the same numbers the
 * 6.6 arch/arm64/gunyah/gunyah_hypercall.c uses.
 */

#ifndef _GUNYAH_HVC_COMPAT_H
#define _GUNYAH_HVC_COMPAT_H

#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * Hypervisor error codes. These match gh_errno.h in the vendor tree; they
 * are repeated here because the 6.6 enum form is needed by the switch in
 * gh_vcpu_run().
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
	GH_ERROR_CSPACE_CAP_INSUF_RIGHTS	= 53,
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
	case GH_ERROR_CSPACE_CAP_INSUF_RIGHTS:
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

#define GH_HYPERCALL(fn)	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
						   ARM_SMCCC_SMC_64, \
						   ARM_SMCCC_OWNER_VENDOR_HYP, \
						   fn)

#define GH_HYPERCALL_HYP_IDENTIFY	GH_HYPERCALL(0x8000)
#define GH_HYPERCALL_BELL_SEND		GH_HYPERCALL(0x8012)
#define GH_HYPERCALL_BELL_SET_MASK	GH_HYPERCALL(0x8015)
#define GH_HYPERCALL_MSGQ_SEND		GH_HYPERCALL(0x801B)
#define GH_HYPERCALL_MSGQ_RECV		GH_HYPERCALL(0x801C)
#define GH_HYPERCALL_VCPU_RUN		GH_HYPERCALL(0x8065)

struct gh_hypercall_hyp_identify_resp {
	u64 api_info;
	u64 flags[3];
};

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

static inline void gh_hypercall_hyp_identify(
	struct gh_hypercall_hyp_identify_resp *hyp_identity)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GH_HYPERCALL_HYP_IDENTIFY, &res);

	hyp_identity->api_info = res.a0;
	hyp_identity->flags[0] = res.a1;
	hyp_identity->flags[1] = res.a2;
	hyp_identity->flags[2] = res.a3;
}

static inline enum gh_error gh_hypercall_bell_send(u64 capid, u64 new_flags,
						   u64 *old_flags)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GH_HYPERCALL_BELL_SEND, capid, new_flags, 0, &res);

	if (res.a0 == GH_ERROR_OK && old_flags)
		*old_flags = res.a1;

	return res.a0;
}

static inline enum gh_error gh_hypercall_bell_set_mask(u64 capid,
						       u64 enable_mask,
						       u64 ack_mask)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(GH_HYPERCALL_BELL_SET_MASK, capid, enable_mask,
			  ack_mask, 0, &res);

	return res.a0;
}

static inline enum gh_error gh_hypercall_vcpu_run(
	u64 capid, u64 *resume_data, struct gh_hypercall_vcpu_run_resp *resp)
{
	struct arm_smccc_1_2_regs args = {
		.a0 = GH_HYPERCALL_VCPU_RUN,
		.a1 = capid,
		.a2 = resume_data[0],
		.a3 = resume_data[1],
		.a4 = resume_data[2],
		/* C says this is implicitly zero; Gunyah requires 0. */
		.a5 = 0,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_hvc(&args, &res);

	if (res.a0 == GH_ERROR_OK) {
		resp->sized_state = res.a1;
		resp->state_data[0] = res.a2;
		resp->state_data[1] = res.a3;
		resp->state_data[2] = res.a4;
	}

	return res.a0;
}

#endif /* _GUNYAH_HVC_COMPAT_H */
