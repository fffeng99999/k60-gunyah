# 第三阶段：内核树内 VM manager 集成设计

生成时间：2026-10-18
状态：设计阶段，尚未开始编码

## 0. 本文的前提

第二阶段实测结论（见 `PHASE2_ANALYSIS.md` 附录 A）：

| 事实 | 证据 |
|---|---|
| RM 允许为客户 VM 分配 VMID | `alloc_vmid: ret=0 vmid=45` |
| RM 拒绝 primary 对该 VMID 执行 reset | `gh_rm_vm_reset(45) ret=-1` |
| RM 拒绝 primary 对该 VMID 执行 deallocate | `gh_rm_vm_dealloc_vmid(45) ret=-1` |
| 每个 VM 名称只能分配一次 | 重复请求 `GH_OEM_VM` 返回 `-22` |
| `/dev/gunyah` 可由树外模块创建 | `device /dev/gunyah created` |
| 厂商模块在新内核上全部正常加载 | `/proc/modules` 386 个模块 |

因此第三阶段的目标是：把 VM manager 集成进内核源码树，用内核态直接发送
RM RPC，而不是继续在树外模块上叠加。

本文记录从源码分析得到的**设计输入**，不包含实现。

---

## 1. 两个 RM 代的差异（核心风险）

设备内核是 5.10 厂商 RM，crosvm（DroidVM 预编译产物）与上游 6.6 VM manager
走的是另一套 RM 消息集。必须分开处理。

### 1.1 设备 5.10 厂商 RM 已有的消息 ID

来自 `tools/src/crdroid/gh_rm_drv_private.h`（= 设备 `gh_rm_drv` 的私有头）：

```text
VM_ALLOCATE        0x56000001
VM_DEALLOCATE      0x56000002
VM_START           0x56000004
VM_STOP            0x56000005
VM_RESET           0x56000006
VM_GET_ID          0x56000010
VM_LOOKUP_URI      0x56000011
VM_LOOKUP_GUID     0x56000012
VM_LOOKUP_NAME     0x56000013
VM_GET_STATE       0x56000017
VM_GET_HYP_RESOURCES 0x56000020
VM_LOOKUP_HYP_CAPIDS 0x56000021
VM_LOOKUP_HYP_IRQS   0x56000022
VM_IRQ_ACCEPT      0x56000050
VM_IRQ_LEND        0x56000051
VM_IRQ_RELEASE     0x56000052
VM_IRQ_RECLAIM     0x56000053
VM_IRQ_NOTIFY      0x56000054
VM_IRQ_UNMAP       0x56000055
VM_SET_STATUS      0x56000080
VM_CONSOLE_OPEN    0x56000081
VM_CONSOLE_CLOSE   0x56000082
VM_CONSOLE_WRITE   0x56000083
VM_CONSOLE_FLUSH   0x56000084
VM_HOST_GET_TYPE   0x560000A0
```

内存类消息（`MEM_*` 0x5100xxxx）设备侧完整存在：
`MEM_DONATE/ACCEPT/LEND/SHARE/RELEASE/RECLAIM/NOTIFY/LOOKUP_SGL`。

### 1.2 6.6 版 VM manager 需要但设备 RM 没有的消息 ID

来自 `tools/src/gunyah-6.6/drivers/virt/gunyah/rsc_mgr_rpc.c`：

```text
VM_CONFIG_IMAGE        0x56000009   ← 设备侧不存在
VM_INIT                0x5600000B   ← 设备侧不存在
VM_GET_VMID            0x56000024   ← 设备侧不存在
VM_SET_FIRMWARE_MEM    0x56000032   ← 设备侧不存在
MEM_APPEND             0x51000018   ← 设备侧不存在
```

**这是本阶段最大的技术风险。** `gh_vm_start()`（6.6 版）的调用序列是：

```text
gh_rm_alloc_vmid()          → 设备侧有（VM_ALLOCATE）
gh_rm_mem_lend/share()      → 设备侧有
gh_rm_vm_configure()        → 设备侧没有（VM_CONFIG_IMAGE）
gh_rm_vm_init()             → 设备侧没有（VM_INIT）
gh_rm_get_hyp_resources()   → 设备侧有（VM_GET_HYP_RESOURCES）
gh_rm_vm_start()            → 设备侧有（VM_START）
```

也就是说 6.6 的启动路径在第二步和第三步之间断掉了。**不能直接照搬。**

### 1.3 设备侧真正的启动序列

来自 `tools/src/crdroid/gh_vm_loader_sec.c`（厂商 secure VM loader）：

```text
gh_rm_vm_alloc_vmid(vm_name, &vmid)   → VM_ALLOCATE
[加载镜像到 fw_phys，qcom_scm_pas_auth_and_reset]
gh_rm_populate_hyp_res(vmid, name)    → VM_GET_HYP_RESOURCES + 分发 cap
gh_rm_vm_start(vmid)                  → VM_START
等待 VM_STATUS = RUNNING
```

关键差异：**厂商路径不调用 VM_CONFIG_IMAGE / VM_INIT**，因为镜像加载和 PAS 认证
是在 Gunyah 之外由 `qcom_mdt_load` + `qcom_scm_pas_auth_and_reset` 完成的，
RM 只负责分配 VMID、下发资源、启动。

对非 trusted 客户 VM，我们没有 PAS ID，不能用这条路。但可以推断设备 RM 的
能力边界：

- VM_ALLOCATE + VM_START 可用（alloc 已实测成功）；
- 没有 VM_CONFIG_IMAGE，意味着 RM 不接受"通过 memparcel 提供内核镜像+DTB"的
  启动方式。guest 镜像只能由 hypervisor 从已映射的 stage-2 内存直接取。

### 1.4 crosvm 侧的 UAPI 要求

来自 `tools/src/crosvm-gunyah_sys.rs`（DroidVM 预编译 crosvm 实际使用的 ABI）：

```text
GH_CREATE_VM                  'G' 0x0   返回 VM fd
GH_VM_SET_USER_MEM_REGION     'G' 0x1
GH_VM_SET_DTB_CONFIG          'G' 0x2
GH_VM_START                   'G' 0x3
GH_VM_ADD_FUNCTION            'G' 0x4   创建 vCPU/irqfd/ioeventfd
GH_VCPU_RUN                   'G' 0x5
GH_VCPU_MMAP_SIZE             'G' 0x6
GH_VM_REMOVE_FUNCTION         'G' 0x7
GH_VM_SET_BOOT_CONTEXT        'G' 0xa   ← 6.6 内核树没有，crosvm 有
GH_VM_ANDROID_LEND_USER_MEM   'A' 0x11
GH_VM_ANDROID_SET_FW_CONFIG   'A' 0x12
GH_VM_ANDROID_SHARE_BLOB      'A' 0x14
GH_VM_RECLAIM_REGION          'A' 0x13
GH_VM_ANDROID_MAP_CMA_MEM     'A' 0x15
GH_VM_ANDROID_SET_AUTH_TYPE   'A' 0x16
```

注意 `GH_VM_SET_BOOT_CONTEXT`（'G' 0xa，`struct gh_vm_boot_context`）：
crosvm 用它设置 guest 入口 PC。6.6 内核树里没有这个 ioctl，说明它是
Android 分支补充的。**第三阶段必须自己实现它**，否则 crosvm 无法启动 guest。

另注意 `gh_vcpu_run` 的 union 在 crosvm 侧有第三个成员 `page_fault`
（`GH_VCPU_EXIT_PAGE_FAULT` = 3），6.6 内核树只有 `mmio` 和 `status` 两个。
这是 crosvm 分支的扩展，内核侧要么补上，要么让 3 落进 `unknown gh exit reason`。

---

## 2. 内核态直连 RM msgq 的可行性（已确认可行）

### 2.1 为什么内核态能绕过名称限制

树外模块的限制来自 `gh_rm_vm_alloc_vmid()` 的**客户端侧**实现
（`tools/src/crdroid/gh_rm_iface.c:926`）：

```c
spin_lock(&gh_vm_table_lock);
if (gh_vm_table[vm_name].vmid != GH_VMID_INVAL ||
    vm_name == GH_SELF_VM) {
        pr_err("%s: VM_ALLOCATE already called for this VM\n", __func__);
        return -EINVAL;      ← 这是 -22 的来源
}
```

以及 dealloc 被拒：

```c
int gh_rm_vm_dealloc_vmid(gh_vmid_t vmid)
{
        resp = gh_rm_call(GH_RM_RPC_MSG_ID_CALL_VM_DEALLOCATE, ...);
        if (reply_err_code || IS_ERR(resp)) {
                err = reply_err_code;
                return err;   ← 这是 -1 的来源，RM 侧拒绝
        }
```

注意区分：**-22 是 Linux 客户端检查，-1 是 RM 服务端拒绝。**
`gh_vm_table` 是 `gh_rm_drv` 模块内的静态表，内核态代码可以完全跳过它，
直接构造 RPC 包发给 RM，就像 `gh_rm_call()` 自己做的那样。

### 2.2 内核态发送 RM RPC 需要什么

来自 `tools/src/crdroid/gh_rm_core.c` 的 `gh_rm_send_request()`：

```text
RPC 头（struct gh_rm_rpc_hdr，8 字节 packed）：
    version:4 = 1, hdr_words:4 = 2
    type:2 = REQ(1)/CONT(0), fragments:6 = num_fragments
    seq: u16（idr_alloc_cyclic 分配）
    msg_id: u32
消息体 = hdr + payload，超过 240 - 8 = 232 字节要分片
通过 gh_msgq_send() 发出，最后一包带 GH_MSGQ_TX_PUSH
```

接收侧需要注册 RM notifier 或自己起一个 recv 线程，按 seq 匹配
`gh_rm_call_idr`。**这部分不能复用 `gh_rm_drv` 的 idr**，因为它是模块内
静态的；要么自己维护一张 seq 表，要么更简单：

**方案：内核态模块自己 `gh_msgq_register(GH_MSGQ_LABEL_RM)` 会失败**
（`gh_rm_drv` 已经注册过同一个 label，`gh_msgq_register` 明确拒绝重复
注册："Multiple clients cannot register to the same label"）。

所以正确做法是：**在自定义内核树里修改 `gh_rm_drv` 本身**，把
`gh_rm_call()` 改成导出符号，并新增一组内核态 API：
`gh_rm_vm_alloc_vmid_any()` / `gh_rm_vm_dealloc_vmid_force()`。
这就是"集成进内核源码树"的实际含义，而不是另起炉灶。

### 2.3 具体要改的文件

| 文件 | 改动 |
|---|---|
| `drivers/virt/gunyah/gh_rm_core.c` | `EXPORT_SYMBOL(gh_rm_call)`；新增任意 VMID 分配 |
| `drivers/virt/gunyah/gh_rm_iface.c` | 新增 `gh_rm_vm_alloc_vmid_any(u16 *vmid)` |
| `drivers/virt/gunyah/gh_rm_drv_private.h` | 新增消息 ID 定义 |
| `drivers/virt/gunyah/vm_mgr.c`（新增） | 从 6.6 回移 |
| `drivers/virt/gunyah/vm_mgr_mm.c`（新增） | 从 6.6 回移 |
| `drivers/virt/gunyah/gunyah_vcpu.c`（新增） | 从 6.6 回移 + boot_context |
| `include/uapi/linux/gunyah.h`（新增） | 采用 crosvm 侧 ABI，不用 6.6 的 |
| `drivers/virt/gunyah/Kconfig` | 新增 `CONFIG_GH_VM_MGR` |

**保留不动的**：`gh_arm_drv`、`gh_msgq`、`gh_dbl`、`gh_ctrl`、
`gh_virtio_backend`、`gh_irq_lend`、`gh_mem_notifier`、`gh_virt_wdt`。
这些是 DRM/qrtr/virtio 的依赖。

---

## 3. vCPU 的资源分发问题

6.6 的 `gh_vcpu_bind()` 依赖 `gh_rm_get_hyp_resources()` 返回
`GH_RESOURCE_TYPE_VCPU` 条目，然后 `gh_vm_add_resource_ticket()` 匹配
label 拿到 cap_id 和 irq，最后用 `gh_hypercall_vcpu_run(capid, ...)`。

但设备侧的 `gh_rm_populate_hyp_res()`（`gh_rm_core.c:879`）对 VCPU 资源
的处理是：

```c
case GH_RM_RES_TYPE_VCPU:
        ret = gh_rm_get_vm_name(vmid, &vm_name_index);
        if (ret) break;
        if (gh_vcpu_affinity_set_fn[vm_name_index])
                ret = gh_vcpu_affinity_set_fn[vm_name_index](vmid, label, cap_id);
        break;
```

也就是说：**设备侧的 hyp_res 分发走的是"按 VM 名称索引的回调表"**，
不是通用的 resource ticket 机制。而且 `gh_rm_get_vm_name()` 查的是
`gh_vm_table`——我们从任意 VMID 分配路径创建的 VM 不在表里，会直接
`-EINVAL` 退出。

**结论**：6.6 的 resource ticket 机制必须完整移植过来，并且要绕过
`gh_rm_populate_hyp_res()`，在 VM manager 内部直接处理
`VM_GET_HYP_RESOURCES` 的返回。这与 2.2 节的内核态改造是同一件事。

---

## 4. 分步实施计划

每一步都必须能用 `fastboot boot` 临时启动验证，失败重启即回滚，
**不刷任何分区**。

### 第 1 步：内核树内最小 VM manager（只建设备节点）

目标：编译进内核，创建 `/dev/gunyah`，只实现
`GH_CREATE_VM` + `GH_VM_SET_USER_MEM_REGION` + `GH_VM_SET_DTB_CONFIG`，
**不实现启动**。

验证点：

```text
/sys/class/gunyah/ 存在
/dev/gunyah 存在
GH_CREATE_VM 返回 fd
GH_VM_SET_USER_MEM_REGION 成功（走 pin_user_pages）
GH_VM_START 明确返回 -ENOTTY 或 -EOPNOTSUPP（不允许半启动）
厂商模块仍然全部加载
```

这一步能暴露：CFI/LTO 下的模块符号问题、`struct page` 布局差异、
`gh_rm_get()` 的设备引用、SELinux 对 `/dev/gunyah` 的默认标签。

### 第 2 步：任意 VMID 分配 + 生命周期

目标：改 `gh_rm_core.c` / `gh_rm_iface.c`，新增：

```c
int gh_rm_vm_alloc_vmid_any(u16 *vmid);      /* 跳过 gh_vm_table */
int gh_rm_vm_dealloc_vmid_direct(u16 vmid); /* 不走客户端表清理 */
```

实测：连续分配 5 个 VMID、逐个 deallocate、再分配 5 个。
如果 RM 侧对任意 VMID 的 deallocate 仍然 EPERM，则记录为已知限制，
VM 生命周期改为"进程退出即 reset"，靠 `GH_RM_NOTIF_VM_EXITED` 兜底。

### 第 3 步：boot_context + guest 启动路径

目标：实现 `GH_VM_SET_BOOT_CONTEXT` 和启动序列。
这是最不确定的一步，因为设备 RM 没有 `VM_CONFIG_IMAGE`。

两条候选路线，需要在这一步实测判定：

- **路线 X**：直接 `VM_ALLOCATE` → `MEM_LEND`(guest RAM + DTB) →
  `VM_START`，靠 hypervisor 从 stage-2 内存取入口点。需要
  `GH_VM_SET_BOOT_CONTEXT` 设置 PC。
- **路线 Y**：如果路线 X 失败，查找 Gunyah 早期版本的
  `VM_CONFIG_IMAGE` 变体，或接受"只能启动厂商 trustedvm"的限制。

判定标准：guest 是否出现 `GH_RM_NOTIF_VM_STATUS` → `RUNNING`。

### 第 4 步：vCPU / irqfd / ioeventfd

移植 `gunyah_vcpu.c` / `gunyah_irqfd.c` / `gunyah_ioeventfd.c`，
配合第 3 步的资源分发改造。实现 `GH_VCPU_RUN` / `GH_VCPU_MMAP_SIZE`。

同时补 crosvm 需要的 `gh_vcpu_run.page_fault` union 成员和
`GH_VCPU_EXIT_PAGE_FAULT`。

### 第 5 步：crosvm 联调

先用仓库自带的 `scripts/vmmgr_probe.c` 扩展版驱动 ioctl，再用
`tools/src/DroidVM-Prebuilts/extracted/usr/bin/crosvm` 实测。

同时处理：

- `/dev/gunyah` 权限从 `0600` 放宽到 `0660` + 组
- SELinux 规则让 `crosvm` 域能打开（需要 vendor sepolicy 或
  Magisk 模块注入）
- DroidVM `gunyah.yaml` 加入 SM8475

---

## 5. 本阶段明确的否决项

1. **不使用 kprobe / 私有符号查找**。内核态可以直接改源码，没有理由 hook。
2. **不替换厂商 `gh_arm_drv` / `gh_msgq` / `gh_dbl` / `gh_ctrl` /
   `gh_virtio_backend`**。DRM、qrtr、virtio 后端都依赖它们。
3. **不改 `gh_vm_loader_sec.c`**。那是签名的 trusted VM 通道，与此无关。
4. **不刷分区**。全程 `fastboot boot`。
5. **不读 `/sdcard`、用户 `/data`、应用数据、`vm-data`**。
6. **不抓 `logcat` / `dumpsys` / `bugreport`**。只用按关键词过滤的
   `dmesg`。

---

## 6. 需要用户确认的事项

| # | 事项 | 影响 |
|---|---|---|
| 1 | 是否接受"自定义内核只在 `fastboot boot` 下测试，长期不刷分区" | 决定交付形态 |
| 2 | 第 3 步如果路线 X 失败，是否接受阶段性结论"设备 RM 不支持非 trusted 客户 VM 启动" | 决定是否继续 |
| 3 | DroidVM 侧是否允许改 `gunyah.yaml` 加入 SM8475 | 影响联调 |
| 4 | CI runner 是否升级（当前 16 GB，Thin LTO 已到极限） | 影响构建时间 |

---

## 7. 下一步

按第 4 节第 1 步开始：写 `vm_mgr.c` 骨架，只建设备节点和
`GH_CREATE_VM`。这一不依赖任何未验证的 RM 能力，可以先合入并构建。
