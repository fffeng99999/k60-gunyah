/*
 * Userspace probe for the K60 in-tree Gunyah VM manager.
 *
 * Walks the VM lifecycle one ioctl at a time and reports exactly where RM
 * refuses, so a failure is actionable instead of a generic "VM did not
 * start". Each step is guarded: if an earlier step failed the later ones
 * are skipped rather than issuing ioctls on an fd that is not in the right
 * state.
 *
 * Build (on device, via the NDK or a static musl toolchain):
 *   aarch64-linux-musl-gcc -static -o vmmgr_probe vmmgr_probe.c
 *
 * Usage:
 *   ./vmmgr_probe                 # full lifecycle, no guest payload
 *   ./vmmgr_probe <image> <dtb>   # also lend a guest image and DTB
 */
/* SPDX-License-Identifier: GPL-2.0-only */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * The UABI. Keep in lockstep with kernel-intree/uapi_linux_gunyah.h and
 * with the DroidVM crosvm bindings.
 */
#define GH_IOCTL_TYPE		'G'
#define GH_ANDROID_IOCTL_TYPE	'A'

#define GH_CREATE_VM		_IO(GH_IOCTL_TYPE, 0x0)

struct gh_userspace_memory_region {
	uint32_t label;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};

#define GH_VM_SET_USER_MEM_REGION	_IOW(GH_IOCTL_TYPE, 0x1, \
						struct gh_userspace_memory_region)
#define GH_VM_ANDROID_LEND_USER_MEM	_IOW(GH_ANDROID_IOCTL_TYPE, 0x11, \
						struct gh_userspace_memory_region)

struct gh_vm_dtb_config {
	uint64_t guest_phys_addr;
	uint64_t size;
};
#define GH_VM_SET_DTB_CONFIG		_IOW(GH_IOCTL_TYPE, 0x2, struct gh_vm_dtb_config)

#define GH_VM_START			_IO(GH_IOCTL_TYPE, 0x3)

#define GH_VM_BOOT_CONTEXT_REG_SHIFT	8
#define GH_VM_BOOT_REG_SET_PC		1

struct gh_vm_boot_context {
	uint32_t reg;
	uint32_t reserved;
	uint64_t value;
};
#define GH_VM_SET_BOOT_CONTEXT		_IOW(GH_IOCTL_TYPE, 0xa, struct gh_vm_boot_context)

struct gh_fn_desc {
	uint32_t type;
	uint32_t arg_size;
	uint64_t arg;
};
#define GH_VM_ADD_FUNCTION		_IOW(GH_IOCTL_TYPE, 0x4, struct gh_fn_desc)
#define GH_VM_REMOVE_FUNCTION		_IOW(GH_IOCTL_TYPE, 0x7, struct gh_fn_desc)

struct gh_fn_vcpu_arg {
	uint32_t id;
};

struct gh_vcpu_run {
	uint8_t immediate_exit;
	uint8_t padding[7];
	uint32_t exit_reason;
	union {
		struct {
			uint64_t phys_addr;
			uint8_t data[8];
			uint32_t len;
			uint8_t is_write;
		} mmio;
		struct {
			uint32_t status;
			struct {
				uint16_t type;
				uint16_t padding;
				uint32_t reason_size;
				uint8_t reason[8];
			} exit_info;
		} status;
		struct {
			uint64_t phys_addr;
			int32_t attempt;
			uint8_t resume_action;
		} page_fault;
	};
};

#define GH_VCPU_RUN			_IO(GH_IOCTL_TYPE, 0x5)
#define GH_VCPU_MMAP_SIZE		_IO(GH_IOCTL_TYPE, 0x6)

#define GH_FN_VCPU	1

/* Memory flags. */
#define GH_MEM_ALLOW_READ	(1UL << 0)
#define GH_MEM_ALLOW_WRITE	(1UL << 1)
#define GH_MEM_ALLOW_EXEC	(1UL << 2)

#define GH_VCPU_EXIT_MMIO	1
#define GH_VCPU_EXIT_STATUS	2
#define GH_VCPU_EXIT_PAGE_FAULT	3

#define PAGE_SIZE_4K		4096

static int step;
static int failures;

static void report(const char *what, int ret)
{
	if (ret < 0)
		printf("  [%02d] %-34s FAILED  errno=%d (%s)\n",
		       ++step, what, errno, strerror(errno));
	else
		printf("  [%02d] %-34s ok (%d)\n", ++step, what, ret);
}

static int open_gh(void)
{
	int fd = open("/dev/gunyah", O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		printf("FATAL: cannot open /dev/gunyah: %s\n", strerror(errno));
		printf("       Is CONFIG_GH_VM_MGR built in? Is the node 0660 and\n");
		printf("       are you in the right group?\n");
		exit(2);
	}

	printf("opened /dev/gunyah (fd %d)\n", fd);
	return fd;
}

static int create_vm(int gh_fd)
{
	int fd = ioctl(gh_fd, GH_CREATE_VM, 0);

	report("GH_CREATE_VM", fd);
	return fd;
}

static int set_mem_region(int vm_fd, uint32_t label, uint64_t gpa,
			  uint64_t size, void *uaddr, uint32_t flags,
			  int lend)
{
	struct gh_userspace_memory_region region = {
		.label = label,
		.flags = flags,
		.guest_phys_addr = gpa,
		.memory_size = size,
		.userspace_addr = (uint64_t)(uintptr_t)uaddr,
	};
	int ret = ioctl(vm_fd,
			lend ? GH_VM_ANDROID_LEND_USER_MEM
			     : GH_VM_SET_USER_MEM_REGION,
			&region);

	report(lend ? "GH_VM_ANDROID_LEND_USER_MEM"
		    : "GH_VM_SET_USER_MEM_REGION", ret);
	return ret;
}

static int set_dtb_config(int vm_fd, uint64_t gpa, uint64_t size)
{
	struct gh_vm_dtb_config cfg = {
		.guest_phys_addr = gpa,
		.size = size,
	};
	int ret = ioctl(vm_fd, GH_VM_SET_DTB_CONFIG, &cfg);

	report("GH_VM_SET_DTB_CONFIG", ret);
	return ret;
}

static int set_boot_context(int vm_fd, uint32_t reg, uint64_t value)
{
	struct gh_vm_boot_context ctx = {
		.reg = reg,
		.value = value,
	};
	int ret = ioctl(vm_fd, GH_VM_SET_BOOT_CONTEXT, &ctx);

	report("GH_VM_SET_BOOT_CONTEXT", ret);
	return ret;
}

static int start_vm(int vm_fd)
{
	int ret = ioctl(vm_fd, GH_VM_START, 0);

	report("GH_VM_START", ret);
	return ret;
}

static int add_vcpu(int vm_fd, uint32_t id)
{
	struct gh_fn_vcpu_arg arg = { .id = id };
	struct gh_fn_desc desc = {
		.type = GH_FN_VCPU,
		.arg_size = sizeof(arg),
		.arg = (uint64_t)(uintptr_t)&arg,
	};
	int fd = ioctl(vm_fd, GH_VM_ADD_FUNCTION, &desc);

	report("GH_VM_ADD_FUNCTION(GH_FN_VCPU)", fd);
	return fd;
}

static void *map_vcpu_run(int vcpu_fd, size_t *size)
{
	long sz = ioctl(vcpu_fd, GH_VCPU_MMAP_SIZE, 0);

	report("GH_VCPU_MMAP_SIZE", (int)sz);
	if (sz <= 0)
		return NULL;

	*size = (size_t)sz;
	return mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED,
		    vcpu_fd, 0);
}

static void run_vcpu(int vcpu_fd, struct gh_vcpu_run *run, int iterations)
{
	int i;

	for (i = 0; i < iterations; i++) {
		int ret = ioctl(vcpu_fd, GH_VCPU_RUN, 0);

		if (ret < 0) {
			printf("  GH_VCPU_RUN #%d            FAILED  errno=%d (%s)\n",
			       i + 1, errno, strerror(errno));
			failures++;
			return;
		}

		printf("  GH_VCPU_RUN #%d            exit_reason=%u",
		       i + 1, run->exit_reason);

		switch (run->exit_reason) {
		case GH_VCPU_EXIT_MMIO:
			printf(" MMIO %s gpa=%#llx len=%u\n",
			       run->mmio.is_write ? "WRITE" : "READ",
			       (unsigned long long)run->mmio.phys_addr,
			       run->mmio.len);
			if (!run->mmio.is_write) {
				/* Answer the read with something benign. */
				memset(run->mmio.data, 0, sizeof(run->mmio.data));
			}
			break;
		case GH_VCPU_EXIT_STATUS:
			printf(" STATUS status=%u exit_type=%u\n",
			       run->status.status,
			       run->status.exit_info.type);
			return;
		case GH_VCPU_EXIT_PAGE_FAULT:
			printf(" PAGE_FAULT gpa=%#llx attempt=%d\n",
			       (unsigned long long)run->page_fault.phys_addr,
			       run->page_fault.attempt);
			return;
		default:
			printf(" UNKNOWN\n");
			return;
		}
	}
}

/*
 * Map a file into memory for lending. Kept anonymous when no path is given
 * so the probe can exercise the parcel machinery without a real image.
 */
static void *map_file_or_anon(const char *path, size_t size, int *out_fd)
{
	void *addr;
	int fd = -1;

	if (path) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			printf("FATAL: cannot open %s: %s\n", path,
			       strerror(errno));
			exit(2);
		}
	}

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | (path ? 0 : MAP_ANONYMOUS),
		    path ? fd : -1, 0);
	if (addr == MAP_FAILED) {
		printf("FATAL: mmap failed: %s\n", strerror(errno));
		exit(2);
	}

	if (!path)
		memset(addr, 0, size);

	*out_fd = fd;
	return addr;
}

int main(int argc, char **argv)
{
	const char *image = argc > 1 ? argv[1] : NULL;
	const char *dtb = argc > 2 ? argv[2] : NULL;
	int gh_fd, vm_fd, vcpu_fd, image_fd = -1, dtb_fd = -1;
	void *image_map, *dtb_map;
	struct gh_vcpu_run *run;
	size_t run_size;
	const uint64_t ram_size = 128UL << 20;	/* 128 MiB */
	const uint64_t ram_gpa = 0x40000000UL;
	const uint64_t dtb_gpa = 0x48000000UL;

	printf("=== K60 Gunyah VM manager probe ===\n");
	printf("ram: gpa=%#llx size=%llu MiB\n",
	       (unsigned long long)ram_gpa,
	       (unsigned long long)(ram_size >> 20));

	gh_fd = open_gh();

	vm_fd = create_vm(gh_fd);
	if (vm_fd < 0) {
		close(gh_fd);
		return 1;
	}

	/* Guest RAM: LEND so the guest owns it exclusively. */
	image_map = map_file_or_anon(image, ram_size, &image_fd);
	if (set_mem_region(vm_fd, 0, ram_gpa, ram_size, image_map,
			   GH_MEM_ALLOW_READ | GH_MEM_ALLOW_WRITE |
			   GH_MEM_ALLOW_EXEC, 1) < 0) {
		printf("FATAL: guest RAM lend failed; cannot proceed\n");
		goto out;
	}

	if (dtb) {
		dtb_map = map_file_or_anon(dtb, 2UL << 20, &dtb_fd);
		if (set_mem_region(vm_fd, 1, dtb_gpa, 2UL << 20, dtb_map,
				   GH_MEM_ALLOW_READ, 0) < 0)
			printf("warning: DTB region failed\n");
		else if (set_dtb_config(vm_fd, dtb_gpa, 2UL << 20) < 0)
			printf("warning: DTB config failed\n");
	}

	/*
	 * Entry point. A bare Linux arm64 Image starts at its base plus a
	 * small header offset; with no image we just try the base, which
	 * exercises the RM message and reports its verdict.
	 */
	set_boot_context(vm_fd,
			 GH_VM_BOOT_REG_SET_PC << GH_VM_BOOT_CONTEXT_REG_SHIFT,
			 ram_gpa);

	if (start_vm(vm_fd) < 0) {
		printf("FATAL: GH_VM_START failed; VM never ran\n");
		printf("       The lines above show which RM message was\n");
		printf("       refused. That is the blocking capability.\n");
		goto out;
	}

	vcpu_fd = add_vcpu(vm_fd, 0);
	if (vcpu_fd < 0)
		goto out;

	run = map_vcpu_run(vcpu_fd, &run_size);
	if (!run) {
		printf("warning: cannot mmap the vcpu run page\n");
		goto out_vcpu;
	}

	printf("--- running vCPU 0 (up to 20 exits) ---\n");
	run_vcpu(vcpu_fd, run, 20);

	munmap(run, run_size);
out_vcpu:
	close(vcpu_fd);
out:
	close(vm_fd);
	close(gh_fd);
	if (image_fd >= 0)
		close(image_fd);
	if (dtb_fd >= 0)
		close(dtb_fd);

	printf("=== done: %d unexpected failure(s) ===\n", failures);
	return failures ? 1 : 0;
}
