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
| `gh_vm_mgr_compat.h` | 厂商树没有的 6.6 结构体 |
| `gunyah_hvc_compat.h` | vCPU run / bell hypercall |
| `uapi_linux_gunyah.h` | UABI，编号对齐 DroidVM crosvm |

### 1.2 构建与部署

- `scripts/apply_intree.sh`：幂等安装 / `--revert` 卸载
- `scripts/build_kernel.sh`：打补丁 + 设 config + 断言 + 构建 Image 和 modules
- `.github/workflows/build-kernel-vmmgr.yml`：CI
- `scripts/vmmgr_probe.c`：用户态逐步探针

---

## 2. 三轮 CI 的收获

| 轮次 | 结果 | 暴露的问题 |
|---|---|---|
| 1 | 内核编译通过，验证步骤失败 | ①`VM_SET_BOOT_CONTEXT` ID 和结构体是猜的 ②`=m` 无法链接 ③验证逻辑自身错误 |
| 2 | 构建失败 | ④`GH_VM_MGR=y` 根本不可达（依赖墙） |
| 3 | 进行中 | — |

其中 ④ 是最关键的发现，见 `PHASE3_CORRECTIONS3.md`。

---

## 3. 关键设计决策（三轮之后）

### 3.1 `GH_VM_MGR=m`，不是 `=y`

设备 `CONFIG_GH_RM_DRV=m`，`GH_VM_MGR` 依赖它，
所以 `=y` 不可达。VM manager 必须是独立模块。

### 3.2 只导出一个窄接口

`vm_mgr.c` 因此不能调 `gh_rm_call()`。
`gh_rm_vmmgr.c`（编入 `gh_rm_drv`）导出唯一的
`gh_rm_set_vm_boot_context()`，把线上格式封在里面。
其余全部走厂商已导出的具名 API。

### 3.3 `gh_rm_drv.ko` 由我们构建

`gh_rm_vmmgr.o` 挂进 `gh_rm_drv`（无 `module_init`， inert），
所以厂商对象一个字节没改，但模块文件确实换了。
**设备测试时必须用我们的版本覆盖厂商那份**，否则
`gh_vm_mgr.ko` 找不到符号。

---

## 4. 下一步：设备实测

### 4.1 准备

```powershell
# 1. 从 CI 下载 Image + gh_vm_mgr.ko + gh_rm_drv.ko
# 2. 重新打包 boot（保留厂商 ramdisk）
python scripts\repack_boot_v4.py --stock-boot boot_a.img `
  --kernel Image --output boot_vmmgr.img

# 3. 临时启动，不刷分区
.\tools\platform-tools-full\platform-tools\fastboot.exe boot boot_vmmgr.img

# 4. 替换 gh_rm_drv.ko（见 PHASE3_STATUS.md 4.2 节方式 A）
adb root; adb remount
adb push gh_rm_drv.ko /vendor_dlkm/lib/modules/gh_rm_drv.ko
adb push gh_vm_mgr.ko  /vendor_dlkm/lib/modules/gh_vm_mgr.ko
adb reboot
```

### 4.2 验证清单

```text
[ ] sys.boot_completed=1
[ ] SELinux=Enforcing
[ ] uname -r = 5.10.252-gki-g19f54b03cc87
[ ] lsmod 里 gh_rm_drv 存在且 holders 非空（DRM/qrtr/virtio 仍依赖它）
[ ] lsmod 里 gh_vm_mgr 存在
[ ] dmesg 出现 "gh_vm_mgr: Gunyah VM manager loaded"
[ ] /dev/gunyah 存在
[ ] GH_CREATE_VM 返回 fd
[ ] GH_VM_SET_USER_MEM_REGION 成功
[ ] GH_VM_START 的每一步 RM 消息都有明确结果
```

### 4.3 判定点

`GH_VM_START` 会依次触发：

```text
VM_ALLOCATE          已实测可行（Phase 2 拿到 vmid=45）
MEM_LEND / SHARE     未实测
VM_SET_BOOT_CONTEXT  未实测，ID 已与上游核对 = 0x56000031
VM_INIT              未实测，0x5600000B
VM_GET_HYP_RESOURCES 未实测
VM_START             未实测
```

**`VM_INIT` 是生死判据。** RM 不认识它会返回 `rm_err`，
dmesg 打印 `VM_INIT(x) rejected by RM: rm_err=N`。

回滚：`fastboot reboot` 即回到原始内核（临时启动不写分区）。

---

## 5. 风险登记

| 风险 | 状态 |
|---|---|
| `VM_INIT` 设备 RM 是否支持 | **未验证，第 4.3 节判定** |
| `VM_SET_BOOT_CONTEXT` 0x56000031 设备 RM 是否支持 | 未验证，ID 已核对 |
| vCPU cap 的 vIRQ 绑定路径 | 未验证 |
| `gh_rm_drv.ko` 替换后厂商栈是否正常 | 未验证，4.2 节第 5 项专门查 |
| `/dev/gunyah` SELinux 标签 | `miscdevice` 默认 `device`，crosvm 域可能被拒 |
| `GH_VM_LOADER` 启用时节点名冲突 | 已加 `-EEXIST` 明确报错 |
