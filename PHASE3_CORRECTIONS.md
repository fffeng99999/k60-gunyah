# 第三阶段修订：两处必须更正的分析

生成时间：2026-10-18
关联：`PHASE3_PLAN.md`

在写代码之前又核对了一遍厂商源码，发现 `PHASE2_ANALYSIS.md` 第 3.3 节和
`PHASE3_PLAN.md` 第 2.2 节有一个**判断错误**，另一个判断需要补充证据。

---

## 1. 更正：`gh_rm_call()` 不是模块内局部符号

### 之前的判断（错）

> `gh_rm_call()` 是模块内部局部符号，不在 `Module.symvers` 中，也不是稳定
> 导出 API。树外模块无法在正常 Kbuild 约束下调用它。

### 实际情况

`tools/src/crdroid/gh_rm_core.c:649`：

```c
void *gh_rm_call(gh_rm_msgid_t message_id,          /* 没有 static */
		void *req_buff, size_t req_buff_size,
		size_t *resp_buff_size, int *rm_error)
```

`tools/src/crdroid/gh_rm_drv_private.h:392`：

```c
void *gh_rm_call(gh_rm_msgid_t message_id, ...);
```

它是一个**模块内全局符号**（在 `Module.symvers` 里查不到只是因为没写
`EXPORT_SYMBOL`），并且在私有头里有声明。

### 影响

`PHASE3_PLAN.md` 第 2.2 节据此推导出的结论——"必须改 `gh_rm_core.c` 把
`gh_rm_call()` 导出"——是多余的。**任何编译进 `gh_rm_drv` 的源文件都可以
直接调用它。**

这反而让方案更简单：`vm_mgr.c` 用 `gh_rm_drv-y` 挂进 `gh_rm_drv` 的
object 列表，就能直接发 `VM_SET_BOOT_CONTEXT` 这类厂商消息表里没有的
RPC，不需要导出任何符号。

第 2.2 节其余结论（`gh_msgq_register` 拒绝第二个 RM label 客户端）仍然
成立，所以"必须编进 `gh_rm_drv`"这个前提不变，只是理由从"要导出符号"
变成了"要复用 msgq 连接和 idr"。

---

## 2. 补充：VM_ALLOCATE 载荷是原生端序，不是小端

### 6.6 的做法

`tools/src/gunyah-6.6/drivers/virt/gunyah/rsc_mgr_rpc.c`：

```c
struct gh_rm_vm_common_vmid_req {
	__le16 vmid;
	__le16 _padding;
} __packed;
```

### 设备 5.10 厂商的做法

`tools/src/crdroid/gh_rm_drv_private.h`：

```c
struct gh_vm_allocate_req_payload {
	gh_vmid_t vmid;      /* 原生 u16 */
	u16 reserved;
} __packed;

struct gh_vm_allocate_resp_payload {
	u32 vmid;            /* 原生 u32，不是 __le16！ */
} __packed;
```

`VM_START` / `VM_DEALLOCATE` / `VM_RESET` / `VM_GET_HYP_RESOURCES` /
`VM_IRQ_ACCEPT` / `VM_IRQ_RECLAIM` 全部是原生端序。

### 影响

不能把 6.6 的 `struct gh_rm_vm_common_vmid_req` 直接搬过来。必须用厂商
的 payload 结构体，否则：

- `cpu_to_le16()` 在 ARM64 上恰好无害（本机就是小端），但语义错误；
- **响应体是 `u32` 而不是 `__le16`**，照搬 6.6 的
  `struct gh_rm_vm_alloc_vmid_resp` 会读错字段。

已在 `kernel-intree/gh_rm_vmmgr.c` 改用厂商结构体。

---

## 3. 补充：通知载荷也是原生端序

`tools/src/crdroid/include/gh_rm_drv.h`：

```c
struct gh_rm_notif_vm_exited_payload {
	gh_vmid_t vmid;              /* 原生 */
	u16 exit_type;               /* 原生 */
	u32 exit_reason_size;        /* 原生 */
	u32 exit_reason[0];          /* u32 数组，不是 u8 数组 */
} __packed;
```

而我最初按 6.6 的 `struct gh_rm_vm_exited_payload` 写了
`le16_to_cpu(payload->exit_type)` 和 `memcpy(reason, ..., u8[])`。
已改为直接赋值 + `min_t()` 截断。

---

## 4. 补充：通知常量名不同

| 6.6 | 设备 5.10 厂商 |
|---|---|
| `GH_RM_NOTIFICATION_VM_STATUS` | `GH_RM_NOTIF_VM_STATUS` |
| `GH_RM_NOTIFICATION_VM_EXITED` | `GH_RM_NOTIF_VM_EXITED` |
| `struct gh_rm_vm_status_payload` | `struct gh_rm_notif_vm_status_payload` |
| `struct gh_rm_vm_exited_payload` | `struct gh_rm_notif_vm_exited_payload` |

已在全部源文件中替换。

---

## 5. 补充：两处导出符号冲突

`gh_rm_core.c` / `gh_rm_iface.c` 已经导出：

```text
gh_rm_vm_start(gh_vmid_t vmid)
gh_rm_vm_stop(gh_vmid_t vmid, u32 stop_reason, u8 flags)
```

我最初定义的同名函数会造成重复符号。已改名为：

```text
gh_rm_vm_start_raw(u16 vmid)
gh_rm_vm_stop_raw(u16 vmid, u32 stop_reason, u8 flags)
```

---

## 6. 对 PHASE3_PLAN.md 的净影响

| 计划里的说法 | 修正后 |
|---|---|
| "必须改 `gh_rm_core.c` 把 `gh_rm_call()` 导出" | 不需要改，直接调用即可 |
| 第 1 步"不依赖任何未验证的 RM 能力" | 不变，仍然成立 |
| 6.6 载荷结构体可复用 | 不可复用，改用厂商原生端序结构体 |
| `gh_rm_populate_hyp_res()` 必须绕过 | 不变，仍然成立 |

净效果：**改动比原计划更小**，但代码必须逐个字段核对载荷格式，不能照搬
6.6 源码。
