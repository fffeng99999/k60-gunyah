# 第三阶段执行记录

生成时间：2026-10-18
关联：`PHASE3_PLAN.md`、`PHASE3_CORRECTIONS.md`
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
| `gh_rm_msgid_ext.h` | 厂商树没有的 RM 消息 ID |
| `gh_vm_mgr_compat.h` | 厂商树没有的 6.6 结构体 |
| `gunyah_hvc_compat.h` | vCPU run / bell hypercall |
| `uapi_linux_gunyah.h` | UABI，编号对齐 DroidVM crosvm |

### 1.2 构建与部署

- `scripts/apply_intree.sh`：幂等安装 / `--revert` 卸载
- `scripts/build_kernel.sh`：自动应用补丁 + `--enable GH_VM_MGR` + 校验
- `.github/workflows/build-kernel-vmmgr.yml`：CI
- `scripts/vmmgr_probe.c`：用户态逐步探针，每步报告 errno

---

## 2. 编码过程中发现并修掉的缺陷

全部记录在 `PHASE3_CORRECTIONS.md`，摘要：

| # | 问题 | 后果 |
|---|---|---|
| 1 | `gh_rm_call()` 被误判为 static | 多余的导出改造；实际可直接调用 |
| 2 | VM 载荷被当成小端 | 照搬 6.6 会读错 vmid 字段 |
| 3 | 通知载荷被当成小端 + `u8[]` | `exit_type`/`reason` 解析错误 |
| 4 | 通知常量名用 6.6 版 | 编译错误 |
| 5 | `gh_rm_vm_start/stop` 符号冲突 | 重复符号，已改名 `_raw` |
| 6 | `gh_vm_share_blob` 先使用后定义 | 编译错误 |
| 7 | 三处 C90 声明后置语句 | 编译错误 |
| 8 | `gh_vm_free` 对未启动 VM 发 reset | 触发 Phase 2 实测的 EPERM |
| 9 | `gh_vm_start` 未持锁完成检查 | 并发 `GH_VM_START` 泄漏 VMID |
| 10 | 失败路径清 `vm_status` | 跳过 deallocate，泄漏 VMID |
| 11 | `gh_vm_ensure_started` 递归 | 把真实错误掩盖成 `-ENODEV` |

---

## 3. 当前状态

代码已提交并推送：`https://github.com/fffeng99999/k60-gunyah`

CI 正在构建（`build-k60-kernel-vmmgr`）。这一步只验证**能编译**，
不验证设备行为。

---

## 4. 下一步：设备实测

### 4.1 准备

```powershell
# 1. 从 CI 下载 Image
# 2. 重新打包 boot（保留厂商 ramdisk）
python scripts\repack_boot_v4.py --stock-boot boot_a.img `
  --kernel Image --output boot_vmmgr.img

# 3. 临时启动，不刷分区
.\tools\platform-tools-full\platform-tools\fastboot.exe boot boot_vmmgr.img
```

### 4.2 验证清单

```text
[ ] sys.boot_completed=1
[ ] SELinux=Enforcing
[ ] /dev/gunyah 存在
[ ] /proc/modules 厂商模块全部在
[ ] dmesg 出现 "gh_vm_mgr: Gunyah VM manager loaded"
[ ] GH_CREATE_VM 返回 fd
[ ] GH_VM_SET_USER_MEM_REGION 成功
[ ] GH_VM_START 的每一步 RM 消息都有明确结果
```

### 4.3 判定点

`GH_VM_START` 会依次触发：

```text
VM_ALLOCATE          已实测可行（Phase 2 拿到 vmid=45）
MEM_LEND / SHARE     未实测
VM_SET_BOOT_CONTEXT  未实测，Android 私有消息
VM_INIT              未实测，厂商树没有这个消息
VM_GET_HYP_RESOURCES 未实测
VM_START             未实测
```

**`VM_INIT` 是生死判据。** 如果 RM 不认识这个消息，会返回 `rm_err`
并在 dmesg 里打印 `VM_INIT(x) rejected by RM: rm_err=N`。那时需要转到
路线 Y：查找设备 RM 实际接受的启动序列。

回滚：`fastboot reboot` 即回到原始内核（临时启动不写分区）。

---

## 5. 风险登记

| 风险 | 状态 |
|---|---|
| `VM_SET_BOOT_CONTEXT` 消息 ID `0x56000040` 未验证 | 已注释标记，失败时 dmesg 可见 |
| `VM_INIT` 设备 RM 是否支持 | 未验证，第 4.3 节判定 |
| vCPU cap 的 vIRQ 绑定路径 | 未验证，依赖 `gh_rm_vm_bind_virq` |
| Thin LTO + CFI 下 `gh_vm_*` 导出符号 | 未验证，CI 编译通过只解决一半 |
| `/dev/gunyah` SELinux 标签 | `miscdevice` 默认 `device`，crosvm 域可能被拒 |
