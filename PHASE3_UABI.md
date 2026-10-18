# 第三阶段：UABI 选型依据

生成时间：2026-10-18
关联：`PHASE3_CORRECTIONS2.md`

## 问题

上游 Gunyah v17 的内存接口是 `GUNYAH_VM_MAP_MEM` + memfd（guest_memfd 设计），
而设备内核是 5.10 厂商树。选错接口集等于白做。

## 判定方法

不看内核侧有什么，看**用户态实际发什么**。DroidVM 预编译的 crosvm 是固定
二进制，它的 ioctl 序列就是需求规格。

从 `tools/src/crosvm-gunyah-mod.rs` 提取的全部 VM 相关 ioctl：

```text
GH_CREATE_VM
GH_VM_ANDROID_LEND_USER_MEM     ← guest RAM，传用户态地址
GH_VM_SET_USER_MEM_REGION       ← host 可见区域，传用户态地址
GH_VM_ANDROID_MAP_CMA_MEM       ← file-backed 区域，传 fd
GH_VM_SET_DTB_CONFIG
GH_VM_SET_BOOT_CONTEXT
GH_VM_ANDROID_SET_FW_CONFIG
GH_VM_ANDROID_SET_AUTH_TYPE
GH_VM_START
GH_VM_ADD_FUNCTION              ← vCPU / irqfd / ioeventfd
GH_VM_REMOVE_FUNCTION
GH_VM_RECLAIM_REGION
GH_VCPU_RUN
GH_VCPU_MMAP_SIZE
```

**没有** `GUNYAH_VM_MAP_MEM`。

## 结论

内核侧要实现的是"用户态地址 + GUP"这一代，不是 memfd 那一代。
`kernel-intree/vm_mgr.c` 的 `gh_vm_mem_alloc()` 用
`pin_user_pages_fast()` 走 `userspace_addr`，方向正确。

这也意味着 6.6 的 `vm_mgr_mm.c` 不能直接回移——它已经是 memfd 模型。
内存部分是按 5.10 时代的语义重写的。

## 上游启动序列（v17 最早形态）

```text
gunyah_rm_notifier_register
gunyah_rm_alloc_vmid(rm, 0)          → vmid
gunyah_rm_vm_configure(auth, 0,0,0,0,0,0)
gunyah_rm_vm_init(vmid)
gunyah_rm_vm_start(vmid)
```

注意 `vm_configure` 在无镜像时用全零调用。设备 RM 没有这个消息，
所以 `vm_mgr.c` 里跳过了它，改为在下发内存 parcel 之后直接
`VM_SET_BOOT_CONTEXT` + `VM_INIT`。

## 上游 reset 条件（v17 最早形态）

```c
if (ghvm->vm_status == EXITED || READY || INIT_FAILED)
        gunyah_rm_vm_reset(...)
```

`LOAD` 不在其列。这与 Phase 2 实测"RM 对只分配了 VMID 的 VM 拒绝 reset"
一致，也验证了 `vm_mgr.c` 里加的状态保护是对的。
