// SPDX-License-Identifier: GPL-2.0-only
/*
 * 红米 K60（mondrian）Gunyah 只读探测模块。
 *
 * 本模块只调用内核已有的 Gunyah API，并执行一次 hypervisor identify
 * hypercall。它不申请 VM、不映射内存、不修改 hypervisor 状态，也不读取
 * 用户数据或任何用户分区。加载后可随时 rmmod。
 */

#define pr_fmt(fmt) "gunyah_probe: " fmt

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/utsname.h>

#include <linux/gunyah/gh_common.h>
#include <linux/gunyah/gh_rm_drv.h>
#include <asm/gunyah/hcall.h>

/*
 * 与 drivers/virt/gunyah/hcall_ctrl.h 中的私有定义保持一致。
 * 树外模块不能包含私有目录，因此这里只内联所需的 identify hypercall。
 */
struct k60_gh_hyp_identify_resp {
	u64 api_info;
	u64 flags[3];
};

static inline int k60_gh_hcall_hyp_identify(
	struct k60_gh_hyp_identify_resp *resp)
{
	struct gh_hcall_resp _resp = { 0 };

	_gh_hcall(0x6000, (struct gh_hcall_args){ 0 }, &_resp);
	if (resp) {
		resp->api_info = _resp.resp0;
		resp->flags[0] = _resp.resp1;
		resp->flags[1] = _resp.resp2;
		resp->flags[2] = _resp.resp3;
	}

	return 0;
}

#define GUNYAH_EXPECTED_API 1
#define GUNYAH_EXPECTED_VARIANT 81
#define GUNYAH_EXPECTED_PRIMARY_VMID 3

#define GH_API_INFO_API_VERSION(value) (((value) >> 0) & 0x3fff)
#define GH_API_INFO_VARIANT(value) (((value) >> 56) & 0xff)

static const char *vm_name(enum gh_vm_names name)
{
	switch (name) {
	case GH_SELF_VM:
		return "self";
	case GH_PRIMARY_VM:
		return "primary";
	case GH_TRUSTED_VM:
		return "trusted";
	case GH_CPUSYS_VM:
		return "cpusys";
	case GH_OEM_VM:
		return "oem";
	default:
		return "unknown";
	}
}

static void log_vminfo(enum gh_vm_names name)
{
	struct gh_vminfo info = { 0 };
	int ret;

	ret = gh_rm_get_vminfo(name, &info);
	if (ret) {
		pr_warn("gh_rm_get_vminfo(%s) failed: %d\n", vm_name(name), ret);
		return;
	}

	pr_info("vminfo[%s]: guid=%s uri=%s name=%s sign_auth=%s\n",
		vm_name(name),
		info.guid ? "present" : "null",
		info.uri ? info.uri : "(null)",
		info.name ? info.name : "(null)",
		info.sign_auth ? info.sign_auth : "(null)");
}

static int __init gunyah_probe_init(void)
{
	struct k60_gh_hyp_identify_resp hyp = { 0 };
	gh_vmid_t primary_vmid = GH_VMID_INVAL;
	gh_vmid_t self_vmid = GH_VMID_INVAL;
	u32 api;
	u32 variant;
	int ret;

	pr_info("read-only probe loading\n");
	pr_info("kernel=%s\n", utsname()->release);

	ret = k60_gh_hcall_hyp_identify(&hyp);
	if (ret) {
		pr_err("Gunyah identify hypercall failed: %d\n", ret);
		return 0;
	}

	api = GH_API_INFO_API_VERSION(hyp.api_info);
	variant = GH_API_INFO_VARIANT(hyp.api_info);
	pr_info("hypervisor identify: api=%u variant=%u flags=%#llx,%#llx,%#llx\n",
		api, variant, hyp.flags[0], hyp.flags[1], hyp.flags[2]);

	if (api != GUNYAH_EXPECTED_API || variant != GUNYAH_EXPECTED_VARIANT)
		pr_warn("版本与已知基线不同（预期 api=%u variant=%u）\n",
			GUNYAH_EXPECTED_API, GUNYAH_EXPECTED_VARIANT);

	ret = gh_rm_get_vmid(GH_PRIMARY_VM, &primary_vmid);
	if (ret)
		pr_warn("gh_rm_get_vmid(primary) failed: %d\n", ret);
	else
		pr_info("gh_rm_get_vmid(primary): vmid=%u\n", primary_vmid);

	ret = gh_rm_get_vmid(GH_SELF_VM, &self_vmid);
	if (ret)
		pr_warn("gh_rm_get_vmid(self) failed: %d\n", ret);
	else
		pr_info("gh_rm_get_vmid(self): vmid=%u\n", self_vmid);

	if (primary_vmid != GH_VMID_INVAL &&
	    primary_vmid != GUNYAH_EXPECTED_PRIMARY_VMID)
		pr_warn("primary VMID 与已知基线不同（预期 %u）\n",
			GUNYAH_EXPECTED_PRIMARY_VMID);

	if (primary_vmid != GH_VMID_INVAL) {
		ret = gh_rm_get_vm_id_info(primary_vmid);
		pr_info("gh_rm_get_vm_id_info(vmid=%u): ret=%d\n", primary_vmid, ret);
		log_vminfo(GH_PRIMARY_VM);
	}

	log_vminfo(GH_SELF_VM);
	pr_info("probe complete; module has no persistent state and is safe to rmmod\n");
	return 0;
}

static void __exit gunyah_probe_exit(void)
{
	pr_info("read-only probe unloaded\n");
}

module_init(gunyah_probe_init);
module_exit(gunyah_probe_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("k60-gunyah contributors");
MODULE_DESCRIPTION("Read-only Gunyah probe for Redmi K60 (mondrian)");
MODULE_VERSION("0.1.0");
