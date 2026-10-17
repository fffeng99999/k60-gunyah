# 第二阶段可行性分析：为什么只靠当前可加载模块还不能补出 `/dev/gunyah`

生成时间：2026-10-18

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
