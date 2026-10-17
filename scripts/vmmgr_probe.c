
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal userspace helper for the additive K60 Gunyah VM manager.
 * It only opens /dev/gunyah and issues the read-only QUERY_VMINFO
 * ioctl so RM behaviour can be observed without starting a VM.
 *
 * Build for Android:
 *   aarch64-linux-android29-clang -O2 -o vmmgr_probe vmmgr_probe.c
 * Build on Linux:
 *   gcc -O2 -o vmmgr_probe vmmgr_probe.c
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GH_VMMGR_IOC_MAGIC 'g'

struct gh_vmmgr_vminfo {
    unsigned short primary_vmid;
    unsigned short allocated_vmid;
    unsigned char has_uri;
    unsigned char has_name;
    unsigned char reserved[2];
};

#define GH_VMMGR_IOC_QUERY_VMINFO \
    _IOR(GH_VMMGR_IOC_MAGIC, 0x0, struct gh_vmmgr_vminfo)
#define GH_VMMGR_IOC_ALLOC_VMID _IO(GH_VMMGR_IOC_MAGIC, 0x1)
#define GH_VMMGR_IOC_RELEASE_VMID _IO(GH_VMMGR_IOC_MAGIC, 0x2)

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/dev/gunyah";
    struct gh_vmmgr_vminfo info;
    int fd, ret;
    const char *action = (argc > 2) ? argv[2] : "query";

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        return 2;
    }

    memset(&info, 0, sizeof(info));

    if (!strcmp(action, "alloc")) {
        ret = ioctl(fd, GH_VMMGR_IOC_ALLOC_VMID);
        printf("ALLOC_VMID ret=%d errno=%s\n", ret, ret ? strerror(errno) : "-");
    } else if (!strcmp(action, "release")) {
        ret = ioctl(fd, GH_VMMGR_IOC_RELEASE_VMID);
        printf("RELEASE_VMID ret=%d errno=%s\n", ret, ret ? strerror(errno) : "-");
    } else {
        ret = ioctl(fd, GH_VMMGR_IOC_QUERY_VMINFO, &info);
        if (ret) {
            fprintf(stderr, "QUERY_VMINFO failed: %s\n", strerror(errno));
            close(fd);
            return 3;
        }
        printf("primary_vmid=%u\n", info.primary_vmid);
        printf("allocated_vmid=%u\n", info.allocated_vmid);
        printf("has_uri=%u has_name=%u\n", info.has_uri, info.has_name);
    }

    close(fd);
    return 0;
}
