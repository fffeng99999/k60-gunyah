// Field-offset verification for the Gunyah UABI.
// Compares the offsets crosvm's bindgen output implies against our header.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Mirror of kernel-intree/uapi_linux_gunyah.h */
struct gh_userspace_memory_region {
	uint32_t label; uint32_t flags;
	uint64_t guest_phys_addr; uint64_t memory_size; uint64_t userspace_addr;
};
struct gh_vm_dtb_config { uint64_t guest_phys_addr; uint64_t size; };
struct gh_fn_vcpu_arg { uint32_t id; };
struct gh_fn_irqfd_arg { uint32_t fd; uint32_t label; uint32_t flags; uint32_t padding; };
struct gh_fn_ioeventfd_arg {
	uint64_t datamatch; uint64_t addr; uint32_t len; int32_t fd; uint32_t flags; uint32_t padding;
};
struct gh_fn_desc { uint32_t type; uint32_t arg_size; uint64_t arg; };
struct gh_vm_exit_info { uint16_t type; uint16_t padding; uint32_t reason_size; uint8_t reason[8]; };
struct gh_vcpu_run {
	uint8_t immediate_exit; uint8_t padding[7]; uint32_t exit_reason;
	union {
		struct { uint64_t phys_addr; uint8_t data[8]; uint32_t len; uint8_t is_write; } mmio;
		struct { uint32_t status; struct gh_vm_exit_info exit_info; } status;
		struct { uint64_t phys_addr; int32_t attempt; uint8_t resume_action; } page_fault;
	};
};
struct gh_vm_firmware_config { uint64_t guest_phys_addr; uint64_t size; };
struct gunyah_address_range { uint64_t guest_phys_addr; uint64_t size; };
struct gh_vm_share_blob {
	uint32_t label; uint32_t flags; uint64_t guest_phys_addr; uint64_t memory_size;
	uint64_t userspace_addr; uint32_t mem_handle; uint32_t padding;
};
struct gunyah_map_cma_mem_args {
	uint32_t label; uint64_t guest_addr; uint32_t flags; uint32_t guest_mem_fd; uint64_t offset; uint64_t size;
};
struct gunyah_qtvm_auth_arg { uint16_t vm_id; uint32_t pas_id; uint64_t guest_phys_addr; uint64_t size; };
struct gunyah_auth_desc { uint32_t type_; uint32_t arg_size; uint64_t arg; };
struct gh_vm_boot_context { uint32_t reg; uint32_t reserved; uint64_t value; };

#define P(s, f) printf("  %-38s %s = %3zu (size %zu)\n", #s, #f, offsetof(s, f), sizeof(((s*)0)->f))

int main(void)
{
	printf("gh_userspace_memory_region (total %zu):\n", sizeof(struct gh_userspace_memory_region));
	P(struct gh_userspace_memory_region, label);
	P(struct gh_userspace_memory_region, flags);
	P(struct gh_userspace_memory_region, guest_phys_addr);
	P(struct gh_userspace_memory_region, memory_size);
	P(struct gh_userspace_memory_region, userspace_addr);

	printf("gh_vm_dtb_config (total %zu):\n", sizeof(struct gh_vm_dtb_config));
	P(struct gh_vm_dtb_config, guest_phys_addr);
	P(struct gh_vm_dtb_config, size);

	printf("gh_fn_desc (total %zu):\n", sizeof(struct gh_fn_desc));
	P(struct gh_fn_desc, type);
	P(struct gh_fn_desc, arg_size);
	P(struct gh_fn_desc, arg);

	printf("gh_vcpu_run (total %zu):\n", sizeof(struct gh_vcpu_run));
	P(struct gh_vcpu_run, immediate_exit);
	P(struct gh_vcpu_run, exit_reason);
	P(struct gh_vcpu_run, mmio);
	P(struct gh_vcpu_run, status);
	P(struct gh_vcpu_run, page_fault);
	printf("    mmio.phys_addr  = %zu\n", offsetof(struct gh_vcpu_run, mmio.phys_addr));
	printf("    mmio.data       = %zu\n", offsetof(struct gh_vcpu_run, mmio.data));
	printf("    mmio.len        = %zu\n", offsetof(struct gh_vcpu_run, mmio.len));
	printf("    mmio.is_write   = %zu\n", offsetof(struct gh_vcpu_run, mmio.is_write));
	printf("    status.status   = %zu\n", offsetof(struct gh_vcpu_run, status.status));
	printf("    status.exit_info= %zu\n", offsetof(struct gh_vcpu_run, status.exit_info));
	printf("    page_fault.phys = %zu\n", offsetof(struct gh_vcpu_run, page_fault.phys_addr));
	printf("    page_fault.att  = %zu\n", offsetof(struct gh_vcpu_run, page_fault.attempt));

	printf("gh_vm_exit_info (total %zu):\n", sizeof(struct gh_vm_exit_info));
	P(struct gh_vm_exit_info, type);
	P(struct gh_vm_exit_info, reason_size);
	P(struct gh_vm_exit_info, reason);

	printf("gh_vm_share_blob (total %zu):\n", sizeof(struct gh_vm_share_blob));
	P(struct gh_vm_share_blob, label);
	P(struct gh_vm_share_blob, flags);
	P(struct gh_vm_share_blob, guest_phys_addr);
	P(struct gh_vm_share_blob, memory_size);
	P(struct gh_vm_share_blob, userspace_addr);
	P(struct gh_vm_share_blob, mem_handle);

	printf("gunyah_map_cma_mem_args (total %zu):\n", sizeof(struct gunyah_map_cma_mem_args));
	P(struct gunyah_map_cma_mem_args, label);
	P(struct gunyah_map_cma_mem_args, guest_addr);
	P(struct gunyah_map_cma_mem_args, flags);
	P(struct gunyah_map_cma_mem_args, guest_mem_fd);
	P(struct gunyah_map_cma_mem_args, offset);
	P(struct gunyah_map_cma_mem_args, size);

	printf("gunyah_auth_desc (total %zu):\n", sizeof(struct gunyah_auth_desc));
	P(struct gunyah_auth_desc, type_);
	P(struct gunyah_auth_desc, arg_size);
	P(struct gunyah_auth_desc, arg);

	printf("gunyah_qtvm_auth_arg (total %zu):\n", sizeof(struct gunyah_qtvm_auth_arg));
	P(struct gunyah_qtvm_auth_arg, vm_id);
	P(struct gunyah_qtvm_auth_arg, pas_id);
	P(struct gunyah_qtvm_auth_arg, guest_phys_addr);
	P(struct gunyah_qtvm_auth_arg, size);

	printf("gh_vm_boot_context (total %zu):\n", sizeof(struct gh_vm_boot_context));
	P(struct gh_vm_boot_context, reg);
	P(struct gh_vm_boot_context, reserved);
	P(struct gh_vm_boot_context, value);

	printf("gh_fn_irqfd_arg (total %zu):\n", sizeof(struct gh_fn_irqfd_arg));
	P(struct gh_fn_irqfd_arg, fd);
	P(struct gh_fn_irqfd_arg, label);
	P(struct gh_fn_irqfd_arg, flags);

	printf("gh_fn_ioeventfd_arg (total %zu):\n", sizeof(struct gh_fn_ioeventfd_arg));
	P(struct gh_fn_ioeventfd_arg, datamatch);
	P(struct gh_fn_ioeventfd_arg, addr);
	P(struct gh_fn_ioeventfd_arg, len);
	P(struct gh_fn_ioeventfd_arg, fd);
	P(struct gh_fn_ioeventfd_arg, flags);
	return 0;
}
