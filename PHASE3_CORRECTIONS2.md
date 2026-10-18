# 第三阶段补充修订：CI 首轮暴露的问题

生成时间：2026-10-18
关联：`PHASE3_CORRECTIONS.md`

第一次 CI 运行（run `39109228344`）的结果：

```text
Apply in-tree VM manager      completed success
Build arm64 Image with VM manager  completed success   ← 内核编译通过
Verify Image and config        completed failure
```

内核**编译通过了**。失败在验证步骤，原因是我自己的检查写错了。但这次运行
暴露了三个真实缺陷。

---

## 1. `VM_SET_BOOT_CONTEXT` 的消息 ID 和结构体都是猜的（最严重）

### 我猜的

```c
#define GH_RM_RPC_MSG_ID_CALL_VM_SET_BOOT_CONTEXT	0x56000040

struct gh_vm_set_boot_ctx_req {
	__le16 vmid;
	__le16 reserved;
	__le32 reg;
	__le64 value;
} __packed;
```

### 上游 v17 真实定义

从 `tools/src/gunyah-v17-full.mbox` 的 `rsc_mgr_rpc.c` hunk 里提取：

```c
#define GUNYAH_RM_RPC_VM_SET_BOOT_CONTEXT	0x56000031

struct gunyah_rm_vm_set_boot_context_req {
	__le16 vmid;
	u8 reg_set;
	u8 reg_index;
	__le32 _padding;
	__le64 value;
} __packed;
```

两处都错：

- ID 是 `0x56000031`，不是 `0x56000040`；
- `reg` 不是单个 `u32`，而是 `reg_set` 和 `reg_index` **两个 u8**。
  UABI 侧把它们打包成 `(reg_set << 8) | reg_index`，驱动下发前要拆开。

这条消息是 guest 启动的唯一入口点指定手段，猜错等于整条路线失效。
已修正，并把全部 7 个扩展消息 ID 与 mbox 逐一核对（见下）。

---

## 2. `vm_mgr.o` 无法作为独立模块链接

### 问题

`vm_mgr.c` 调用 `gh_rm_call()`。这个符号在 `gh_rm_core.c` 里不是 static、
也没有 `EXPORT_SYMBOL`——它是**模块内全局符号**。

`GH_VM_MGR=m` 时：

```text
gh_vm_mgr.ko  包含 vm_mgr.o   →  调用 gh_rm_call()  →  符号未导出，链接失败
gh_rm_drv.ko  包含 gh_rm_vmmgr.o  →  同模块内调用，没问题
```

CI 只跑了 `make Image`，没跑 `make modules`，所以没暴露。

### 修法

`GH_VM_MGR=y`，编译进 vmlinux。这样：

- `vm_mgr.c` 调用 `gh_rm_call()` 直接解析到内核镜像内的定义；
- 不需要给厂商驱动新增任何导出符号；
- **厂商 `gh_rm_drv.ko` 保持原样**，DRM / qrtr / virtio 后端不受影响。

`gh_rm_vmmgr.o` 仍然挂进 `gh_rm_drv`（它需要 msgq 连接、idr 和 notifier
链，这些都是那个模块私有的）。

### 附带影响

原计划里"先出只建设备节点的版本用 insmod 验证"这个路径没有了。
VM manager 变成内置，每次测试都要重新 `fastboot boot` 整个 boot.img。
考虑到重启即回滚、不刷分区，这个代价可以接受。

---

## 3. 验证步骤自身写错

```bash
# 错误：GH_VM_MGR=m 时代码不在 Image 里
strings out/kernel/Image | grep -m1 'gh_vm_mgr'
```

已改为检查 `CONFIG_GH_VM_MGR=y` 并断言 vmlinux 里有 `gh_vm_mgr_init`。

---

## 4. 另外修掉的两个小问题

- `gunyah_vcpu.c` 里用了不存在的 `GH_VCPU_STATE_MMIO_READ`，
  应为 `GH_VCPU_ADDRSPACE_VMMIO_READ`（上游枚举名）。
- `gh_vmmgr_hyp_res_entry.partner_vmid` 我写成 `__le16`，
  厂商定义是 `gh_vmid_t`（原生 u16）。ARM64 小端上恰好无害，但是错的。

---

## 5. 消息 ID 核对结果

全部 7 个与上游 v17 mbox 一致：

```text
MATCH  VM_INIT               0x5600000B
MATCH  VM_CONFIG_IMAGE       0x56000009
MATCH  VM_GET_VMID           0x56000024
MATCH  VM_SET_BOOT_CONTEXT   0x56000031
MATCH  VM_SET_DEMAND_PAGING  0x56000033
MATCH  VM_SET_ADDRESS_LAYOUT 0x56000034
MATCH  MEM_APPEND            0x51000018
```

这条核对以后每次改动都要重跑，不能再靠推断。
