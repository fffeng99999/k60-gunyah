// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah ioeventfd VM function for the K60 in-tree VM manager.
 *
 * Lets userspace register an eventfd that fires when the guest accesses a
 * specific guest physical address, so a device model can react without
 * taking a full vCPU exit. Ported from upstream 6.6 unchanged.
 */

#define pr_fmt(fmt) "gh_ioeventfd: " fmt

#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gunyah/gh_common.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

struct gh_ioeventfd {
	struct gh_vm_function_instance *f;
	struct gh_vm_io_handler io_handler;

	struct eventfd_ctx *ctx;
};

static int gh_write_ioeventfd(struct gh_vm_io_handler *io_dev, u64 addr,
			      u32 len, u64 data)
{
	struct gh_ioeventfd *iofd = container_of(io_dev, struct gh_ioeventfd,
						 io_handler);

	eventfd_signal(iofd->ctx, 1);
	return 0;
}

static struct gh_vm_io_handler_ops io_ops = {
	.write = gh_write_ioeventfd,
};

static long gh_ioeventfd_bind(struct gh_vm_function_instance *f)
{
	const struct gh_fn_ioeventfd_arg *args = f->argp;
	struct gh_ioeventfd *iofd;
	struct eventfd_ctx *ctx;
	int ret;

	if (f->arg_size != sizeof(*args))
		return -EINVAL;

	if (args->flags & ~GH_IOEVENTFD_FLAGS_DATAMATCH)
		return -EINVAL;

	/* must be a natural word size, or 0 to ignore length */
	switch (args->len) {
	case 0:
	case 1:
	case 2:
	case 4:
	case 8:
		break;
	default:
		return -EINVAL;
	}

	if (args->addr + args->len < args->addr)
		return -EINVAL;

	/* a zero-length ioeventfd cannot use datamatch */
	if (!args->len && (args->flags & GH_IOEVENTFD_FLAGS_DATAMATCH))
		return -EINVAL;

	ctx = eventfd_ctx_fdget(args->fd);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	iofd = kzalloc(sizeof(*iofd), GFP_KERNEL);
	if (!iofd) {
		ret = -ENOMEM;
		goto err_eventfd;
	}

	f->data = iofd;
	iofd->f = f;
	iofd->ctx = ctx;

	if (args->flags & GH_IOEVENTFD_FLAGS_DATAMATCH) {
		iofd->io_handler.datamatch = true;
		iofd->io_handler.len = args->len;
		iofd->io_handler.data = args->datamatch;
	}
	iofd->io_handler.addr = args->addr;
	iofd->io_handler.ops = &io_ops;

	ret = gh_vm_add_io_handler(f->ghvm, &iofd->io_handler);
	if (ret)
		goto err_io_dev_add;

	return 0;

err_io_dev_add:
	kfree(iofd);
err_eventfd:
	eventfd_ctx_put(ctx);
	return ret;
}

static void gh_ioevent_unbind(struct gh_vm_function_instance *f)
{
	struct gh_ioeventfd *iofd = f->data;

	eventfd_ctx_put(iofd->ctx);
	gh_vm_remove_io_handler(iofd->f->ghvm, &iofd->io_handler);
	kfree(iofd);
}

static bool gh_ioevent_compare(const struct gh_vm_function_instance *f,
			       const void *arg, size_t size)
{
	const struct gh_fn_ioeventfd_arg *instance = f->argp, *other = arg;

	if (sizeof(*other) != size)
		return false;

	return instance->addr == other->addr;
}

static struct gh_vm_function gh_ioeventfd_function = {
	.type = GH_FN_IOEVENTFD,
	.name = "ioeventfd",
	.mod = THIS_MODULE,
	.bind = gh_ioeventfd_bind,
	.unbind = gh_ioevent_unbind,
	.compare = gh_ioevent_compare,
};

int gh_ioeventfd_function_init(void)
{
	return gh_vm_function_register(&gh_ioeventfd_function);
}

void gh_ioeventfd_function_exit(void)
{
	gh_vm_function_unregister(&gh_ioeventfd_function);
}
