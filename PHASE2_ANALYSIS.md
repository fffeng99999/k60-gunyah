# 第二阶段可行性分析：VM manager 集成路径

生成时间：2026-10-18
更新：2026-10-18（设备实测后修订）

## 0. 结论摘要

**设备 Resource Manager 允许分配非 trusted 客户 VM。**

在自编译 5.10.252 内核上加载 `gh_vmmgr` 并执行 RM VMID 分配，得到：

```text
gh_vmmgr: RM allocated vmid=45
gh_vmmgr: alloc_vmid: ret=0 vmid=45
```

这推翻了本文早前“RM 可能只允许 trusted VM”的假设。`/dev/gunyah` 已由
`gh_vmmgr` 创建成功（major 461），设备开机、SELinux、厂商模块均正常。

实测确认阻塞点有三层：

1. **VMID 分配可以，但回收不行**：`gh_rm_vm_alloc_vmid(GH_OEM_VM)` 成功返回
   VMID 45，但 `gh_rm_vm_reset()` 和 `gh_rm_vm_dealloc_vmid()` 均返回 EPERM；
2. **每个 VM 名称只能分配一次**：释放失败后再次分配同一名称返回 EINVAL，
   因此现有 `gh_rm_*` API 无法创建多个客户 VM；
3. **启动 VM 的接口缺失**：没有 vCPU run、guest memory lend/share 配置、
   boot context 等 `/dev/gunyah` 完整 UAPI 所需的调用。

因此结论是：**树外模块 + 现有厂商导出 API 不足以实现可用的 VM manager**。
要继续必须把 VM manager 集成进内核源码树（Bootloader 已解锁、原分区已备份、
自编译内核已验证可启动），而不是继续扩展树外模块。

---

## 历史结论（已修订，保留供追溯）

以下第 1-6 节写于设备实测之前，其中关于 RM 只允许 trusted VM 的推测
已被上面的实测数据推翻；关于“树外模块无法补齐 VM manager”的结论仍然
成立，但原因是接口缺失而非 RM 授权。

---

## 1. DroidVM/crosvm 需要什么 UAPI

DroidVM 当前预编译 crosvm（Droid-VM/crosvm `droidvm` 分支）使用 Android Gunyah
VM manager UAPI，核心 ioctl 包括：

```text
GH_CREATE_VM
GH_VM_SET_USER_MEM_REGION
GH_VM_ANDROID_LEND_USER_MEM
GH_VM_SET_DTB_CONFIG
GH_VM_SET_BOOT_CONTEXT
GH_VM_ANDROID_SET_AUTH_TYPE
GH_VM_ANDROID_SET_FW_CONFIG
GH_VM_ADD_FUNCTION
GH_VM_REMOVE_FUNCTION
GH_VM_START
GH_VCPU_RUN
GH_VCPU_MMAP_SIZE
```

`GH_CREATE_VM` 必须返回一个独立 VM fd；后续 ioctl 在该 fd 上完成 VM 配置、内存
lend/share、vCPU 创建和启动。参考：

- `hypervisor/src/gunyah/gunyah_sys.rs`
- `hypervisor/src/gunyah/gunyah_sys/bindings.rs`
- `hypervisor/src/gunyah/mod.rs`

## 2. 设备现有内核导出了什么

设备 `gh_rm_drv` 导出了：

```text
gh_rm_get_vmid
gh_rm_get_vminfo
gh_rm_get_vm_id_info
gh_rm_vm_alloc_vmid
gh_rm_vm_dealloc_vmid
gh_rm_vm_start
gh_rm_vm_stop
gh_rm_vm_reset
gh_rm_mem_share
gh_rm_mem_lend
gh_rm_mem_donate
gh_rm_mem_accept
gh_rm_mem_release
gh_rm_mem_reclaim
gh_rm_vm_irq_*
```

`gh_arm_drv` 只导出 IRQ 相关函数，不导出 vCPU run。设备也没有 `vm_mgr.c`、
`gunyah_vcpu.c`、`guest_memfd.c` 或对应字符设备层。

因此树外模块缺少以下关键能力：

```text
任意 VMID 分配
VM boot context 设置
DTB / firmware 配置
vCPU capability 创建
GH_VCPU_RUN
uAPI VM 对象生命周期
通用 RM 请求/应答入口
```

## 3. 为什么不能直接复用现有导出函数

### 3.1 `gh_rm_vm_alloc_vmid()` 不是通用分配器

当前 5.10 厂商实现把 VMID 表限制为固定枚举：

```c
enum gh_vm_names {
	GH_SELF_VM,
	GH_PRIMARY_VM,
	GH_TRUSTED_VM,
	GH_CPUSYS_VM,
	GH_OEM_VM,
	GH_VM_MAX
};
```

并且：

- `GH_SELF_VM` 被直接拒绝；
- 每个枚举项只允许分配一次；
- 没有任意 VM 名称、URI、auth type 参数；
- 没有为每个客户 VM 保存独立上下文。

这不能满足 crosvm 每次 `GH_CREATE_VM` 创建独立 VM 的模型。

### 3.2 `gh_rm_get_vm_id_info(3)` 实测失败

设备实测：

```text
gh_rm_get_vm_id_info(vmid=3): ret=-22 (-EINVAL)
gh_rm_get_vminfo(primary): guid=null uri=(null) name=(null) sign_auth=(null)
```

即使对 primary VM，RM 也没有返回可用的标识信息。不能把该接口当作后续 VM 管理器
的可靠元数据来源。

### 3.3 通用 RM RPC 未导出

设备过滤后的 `/proc/kallsyms` 显示：

```text
0000000000000000 t gh_rm_call  [gh_rm_drv]
```

`gh_rm_call()` 是模块内部局部符号，不在 `Module.symvers` 中，也不是稳定导出 API。
树外模块无法在正常 Kbuild 约束下调用它。

### 3.4 私有函数 Hook 不可接受

虽然可以用 kprobe 解析 `gh_rm_call` 地址并绕过模块符号检查，但这会带来：

- 依赖精确函数地址/私有结构布局；
- 绕过内核 CFI / LTO / MODVERSIONS 的稳定性边界；
- 内核升级或 ROM 更新后极易崩溃；
- 无法作为 Magisk 长期模块交付。

因此本项目不采用 kprobe 私有符号路线。

### 3.5 厂商 secure VM 通道不能替代

`guestvm_loader` / `vmmgr` 只加载签名的 trusted VM 镜像；设备的
`CONFIG_GH_VM_LOADER` 也未启用。该通道不能运行 DroidVM 生成的 Debian/Windows 镜像。

## 4. 结论

在当前约束下：

- 只读探测模块可以安全加载；
- 仅靠一个 Magisk 可加载模块，**不能可靠补出** crosvm 所需的完整 `/dev/gunyah`
  VM manager；
- 若不修改内核/刷入新 boot 镜像，第二阶段的“创建并启动自定义 VM”没有可接受的安全实现路径。

## 5. 可行路线

### 路线 A：接受修改内核并刷 boot（唯一完整路线）

在 5.10.252 树中集成：

- Android GKI Gunyah VM manager
- vCPU run 与 guest memory lend/share
- Android ioctl 扩展（auth/fw/CMA）
- 与 SM8475 厂商 RM 的适配层

然后重新构建 boot/内核并验证。此路线与当前“不刷内核、不改 boot”约束冲突，需要用户
重新确认。

### 路线 B：寻找同代厂商 VM manager

寻找 SM8475/同代 Android 16 厂商源码中已经存在的非 secure Gunyah VM manager，并
确认其 UAPI 与 DroidVM crosvm 兼容。目前公开 crDroid 5.10 分支没有该层。

### 路线 C：继续只做探测与兼容性验证

保持当前只读模块，继续收集：

- RM 对不同 VM 类型的错误码；
- Gunyah hypervisor feature flags；
- DroidVM crosvm 实际 UAPI 版本；
- 是否具备 protected VM / pseudo-unprotected VM 路径。

但这条路不能替代真正的 VM manager。

## 6. 建议

在没有新的内核源码或用户明确同意刷 boot 之前，第二阶段应停在“能力验证”和“内核
适配设计”，不应把 kprobe 私有 hook 或未经验证的 VM 分配写进自动加载模块。

如果用户希望继续，需要先确认以下选项之一：

1. 允许构建并刷入自定义 boot 镜像；
2. 提供同版本 ROM 厂商内核源码/VM manager 模块；
3. 只继续做只读能力探测，不承诺创建 VM。

---

## 附录：设备实测记录（2026-10-18）

### A.1 `/dev/gunyah` 创建

```text
gh_vmmgr: Gunyah VM manager (additive) loaded
gh_vmmgr: device /dev/gunyah created; autoload is off by default
```

设备节点：

```text
crw------- 1 root root 461, 0 /dev/gunyah
SELinux: u:object_r:device:s0
```

`gh_rm_drv` 的 holders 中已包含 `gh_vmmgr`，证明依赖关系正确。

### A.2 RM VMID 分配

```text
gh_vmmgr: probe_rm: ret=0 primary_vmid=3 has_uri=0 has_name=0
gh_vmmgr: RM allocated vmid=45
gh_vmmgr: alloc_vmid: ret=0 vmid=45
```

RM 接受了 `GH_OEM_VM` 名称的 VMID 分配请求，返回 VMID 45。

### A.3 VMID 释放失败

```text
gh_vmmgr: gh_rm_vm_dealloc_vmid(45) failed: -1
```

`-1` 是 `gh_rm_call()` 返回的 Linux 负错误码，对应 EPERM。可能原因：

- RM 要求 VM 先进入 `RESET` 状态才允许 deallocate；
- 已启动/初始化的 VM 不能直接 deallocate；
- RM 对该 RPC 的调用者有额外授权要求。

下一步应先对已分配 VMID 调用 `gh_rm_vm_reset()`，再尝试 deallocate。

### A.3.0 reset-first 实测（2026-10-18，重启后复测）

重启清除残留状态后重新分配，并显式先 reset：

```text
gh_vmmgr: RM allocated vmid=45
gh_vmmgr: alloc_vmid: ret=0 vmid=45
gh_vmmgr: reset_vmid: gh_rm_vm_reset(45) ret=-1
```

卸载时走 reset + deallocate：

```text
gh_vmmgr: gh_rm_vm_reset(45) failed: -1
gh_vmmgr: gh_rm_vm_dealloc_vmid(45) failed: -1
```

`gh_rm_vm_reset()` 同样返回 EPERM，因此 reset-first 假设不成立。
结论：**RM 允许 primary VM 分配客户 VMID，但拒绝 primary VM 对该 VMID 执行
reset 和 deallocate**。这与 RM 只信任特定调用者的策略一致，也意味着
`gh_rm_*` 现有导出 API 无法支撑完整的 VM 生命周期管理。

### A.3.1 重复分配返回 EINVAL

在 VMID 45 未被成功释放的情况下再次加载模块并请求分配：

```text
gh_vmmgr: gh_rm_vm_alloc_vmid(GH_OEM_VM) failed: -22
gh_vmmgr: alloc_vmid: ret=-22 vmid=0
```

`-22` 是 EINVAL。这印证了 `gh_rm_vm_alloc_vmid()` 的语义：它按 VM 名称
（`enum gh_vm_names`）分配，每个名称只能持有一次。第一次成功拿到 VMID 45
后 deallocate 失败，`GH_OEM_VM` 名称就被占用，后续同一名称的请求直接失败。

**这是树外模块使用厂商 `gh_rm_*` API 的根本限制**：无法分配任意数量的客户 VM。

### A.4 安全状态

```text
SELinux        = Enforcing
boot_completed = 1
uname -r       = 5.10.252-gki-g19f54b03cc87
```

模块加载/卸载后设备无异常，未影响正常使用。
