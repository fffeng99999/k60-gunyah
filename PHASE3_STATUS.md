# 第三阶段执行记录

生成时间：2026-10-18
关联：`PHASE3_PLAN.md`、`PHASE3_CORRECTIONS*.md`、`PHASE3_UABI.md`
代码：`kernel-intree/`，由 `scripts/apply_intree.sh` 安装进内核树

---

## 1. 已交付

### 1.1 内核态 VM manager（`kernel-intree/`）

| 文件 | 作用 |
|---|---|
| `vm_mgr.c` | `/dev/gunyah`、VM fd 生命周期、全部 ioctl |
| `vm_mgr.h` | 内部类型、hypervisor 资源线上格式 |
| `gunyah_vcpu.c` | `GH_VM_ADD_FUNCTION` / `GH_VCPU_RUN` / mmap |
| `gunyah_irqfd.c` | eventfd → guest doorbell |
| `gunyah_ioeventfd.c` | guest MMIO → eventfd |
| `gh_rm_vmmgr.c` | RM 扩展，编入 `gh_rm_drv` |
| `gh_rm_msgid_ext.h` | 厂商树没有的 RM 消息 ID（全部与上游 v17 核对） |
| `gh_vm_mgr_compat.h` | 厂商树没有的结构体 / 常量 |
| `gunyah_hvc_compat.h` | vCPU run hypercall（走厂商 `_gh_hcall`） |
| `uapi_linux_gunyah.h` | UABI，编号对齐 DroidVM crosvm |

### 1.2 构建与校验

| 文件 | 作用 |
|---|---|
| `scripts/apply_intree.sh` | 幂等安装 / `--revert` |
| `scripts/build_kernel.sh` | 打补丁 + 编译 Image + `M=` 编译模块 + CRC 门禁 |
| `scripts/gen_vendor_decls.ps1` | 从厂商头生成 `gh_vendor_decls.h` |
| `scripts/compare_module_crcs.py` | 模块导出 CRC 与设备对比 |
| `scripts/precompile_check.ps1` | 编译前静态检查（6 类 + 5.10 API + tag 冲突） |
| `scripts/vmmgr_probe.c` | 用户态逐步探针 |
| `.github/workflows/build-kernel-vmmgr.yml` | CI |

---

## 2. 编译前静态检查

`scripts/precompile_check.ps1` 检查：

1. 调用未声明的函数
2. 结构体没有该成员（含匿名 union、函数指针）
3. C90 声明后置语句
4. 括号 / 注释不平衡
5. 与厂商重复的导出符号
6. 与厂商 struct/enum/union **tag 冲突**
7. （`-KernelSrc`）45 个内核 API 在真实 5.10 树中存在

已接入 CI，作为最前面的 gate，秒级失败。

---

## 3. 到目前为止发现并修掉的真实缺陷

| # | 缺陷 | 后果 |
|---|---|---|
| 1 | `gh_rm_call()` 被误判为 static | 方案走偏，实际可直接调用 |
| 2 | VM 载荷被当成小端 | 照搬 6.6 会读错 vmid |
| 3 | 通知载荷端序 + `u32[]` 误判 | exit_type/reason 解析错误 |
| 4 | 通知常量名用 6.6 版 | 编译错误 |
| 5 | `gh_rm_vm_start/stop` 符号冲突 | 重复符号 |
| 6 | `gh_vm_share_blob` 先使用后定义 | 编译错误 |
| 7 | 三处 C90 声明后置 | 编译错误 |
| 8 | `gh_vm_free` 对未启动 VM 发 reset | 触发 Phase 2 实测的 EPERM |
| 9 | `gh_vm_start` 未持锁完成检查 | 并发泄漏 VMID |
| 10 | 失败路径清 `vm_status` | 跳过 deallocate |
| 11 | `ensure_started` 递归 | 掩盖真实错误 |
| 12 | **`arm_smccc_1_2_hvc` 在 5.10 不存在** | 编译错误 |
| 13 | **hypercall 基址 0x8000 vs 设备 0x6000** | vCPU run 功能错误 |
| 14 | **`rb_find` / `rb_find_add` 5.10 不存在** | 编译错误 |
| 15 | **`overflows_type` 5.10 不存在** | 编译错误 |
| 16 | **`enum gh_rm_vm_status` 是宏不是类型** | 编译错误 |
| 17 | **UAPI `enum gh_vm_status` 与厂商 `struct gh_vm_status` tag 冲突** | 编译错误（已实测复现） |
| 18 | vIRQ 未转成 Linux IRQ | `request_irq` 收到 vIRQ 号 |
| 19 | `request_irq` 失败后仍发布 `rsc` | 释放未申请的中断 |
| 20 | vCPU 无 IRQ 仍被接受 | `GH_VCPU_RUN` 永久阻塞 |
| 21 | `gh_vm_stop` 无超时 | workqueue 线程永久挂起 |
| 22 | **ACL 第 0 项 VMID 未填** | parcel 被送给 VMID 0 |
| 23 | **空的 `mem_attr_desc` 被下发** | 厂商函数直接返回 `-EINVAL` |
| 24 | 运行期新增内存区域不下发 | guest 看不到热插拔内存 |
| 25 | 未实现的 ioctl 返回 `-ENOTTY` | 与"不支持 UABI"无法区分 |

---

## 4. 构建方式（重要）

### 4.1 `GH_VM_MGR` 只能是 `=m`

设备 `CONFIG_GH_RM_DRV=m`，`GH_VM_MGR` 依赖它，因此 `=y` 不可达。

### 4.2 模块结构

```text
gh_rm_drv.ko   厂商原有 object + gh_rm_vmmgr.o
gh_vm_mgr.ko   vm_mgr.o + gunyah_vcpu.o + gunyah_irqfd.o + gunyah_ioeventfd.o
```

`gh_rm_drv.ko` 由我们重建并**替换厂商版本**，因为
`gh_rm_vmmgr.o` 必须和 `gh_rm_call()` 在同一个模块里。

**风险**：`msm_drm` / `qrtr_gunyah` / `gh_virtio_backend` 都 import
`gh_rm_drv` 的符号。CRC 变了它们就拒绝加载，设备会没有显示。

**防护**：`scripts/compare_module_crcs.py` 在构建末尾对比我们重建的
`Module.symvers` 与设备 `config/Module.symvers`，任何共用符号 CRC
不一致就构建失败。

### 4.3 只编译 `drivers/virt/gunyah`

全量 `make modules` 在这棵厂商树里会失败：

```text
drivers/clk/qcom/trace.h       TRACE_INCLUDE_PATH "."
drivers/soc/qcom/dcvs/...      TRACE_INCLUDE_PATH "."
=> #include "./trace.h" 相对 include/trace/ 解析，找不到
```

与 Gunyah 无关。用 `M=drivers/virt/gunyah` 绕开。

### 4.4 符号来源用设备 Module.symvers

`gh_rm_drv.ko` import `gh_get_irq`/`gh_put_irq`/`gh_get_virq`/`gh_put_virq`
来自 `gh_arm_drv.ko`（在 `arch/arm64/gunyah/`，不在本次构建范围）。
用设备 `config/Module.symvers` 作符号源（2738 个重叠符号 CRC 0 差异），
否则这些符号 CRC 会是 0，MODVERSIONS 拒绝加载。

---

## 5. 下一步：设备实测

### 5.1 准备

```powershell
python scripts\repack_boot_v4.py --stock-boot boot_a.img `
  --kernel Image --output boot_vmmgr.img
.\tools\platform-tools-full\platform-tools\fastboot.exe boot boot_vmmgr.img
```

### 5.2 模块部署

`gh_rm_drv.ko` 必须替换厂商版本，二选一：

**A. 覆盖 vendor_dlkm**

```powershell
adb root; adb remount
adb push gh_rm_drv.ko /vendor_dlkm/lib/modules/gh_rm_drv.ko
adb push gh_vm_mgr.ko  /vendor_dlkm/lib/modules/gh_vm_mgr.ko
adb reboot
```

**B. Magisk overlay（可回滚，推荐）**

用 Magisk 模块把两个 `.ko` 映射到
`/vendor_dlkm/lib/modules/`，并把 `gh_vm_mgr.ko` 加入 `modules.load`
（或在 `service.sh` 里 `insmod`，此时厂商 `gh_rm_drv` 已加载）。

### 5.3 验证清单

```text
[ ] sys.boot_completed=1
[ ] SELinux=Enforcing
[ ] uname -r = 5.10.252-gki-g19f54b03cc87
[ ] lsmod: gh_rm_drv 存在，holders 非空（DRM/qrtr/virtio 仍依赖）
[ ] lsmod: gh_vm_mgr 存在
[ ] dmesg: "gh_vm_mgr: Gunyah VM manager loaded"
[ ] /dev/gunyah 存在
[ ] probe: GH_CREATE_VM 返回 fd
[ ] probe: GH_VM_SET_USER_MEM_REGION 成功
[ ] probe: GH_VM_START 各步 RM 结果可见
```

### 5.4 判定点

```text
VM_ALLOCATE          已实测可行（Phase 2 拿到 vmid=45）
MEM_LEND / SHARE     未实测
VM_SET_BOOT_CONTEXT  未实测（ID 0x56000031，已与上游 v17 核对）
VM_INIT              未实测（0x5600000B）
VM_GET_HYP_RESOURCES 未实测
VM_START             未实测
```

**`VM_INIT` 是生死判据。** 不支持时 dmesg 会打印
`VM_INIT(x) rejected by RM: rm_err=N`，那一步就是结论。

回滚：`fastboot reboot` 回到原始内核（临时启动不写分区）。
