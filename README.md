# K60 Gunyah Probe

面向 Redmi K60（`mondrian`，SM8475）的 Gunyah 内核模块项目。当前阶段只实现
**只读探测模块**，用于验证：

- 设备内核允许加载自编译树外模块；
- MODVERSIONS CRC 与设备内核匹配；
- Gunyah identify hypercall 与 Resource Manager 查询接口可用；
- 模块可以在不创建 VM、不映射内存的前提下安全加载和卸载。

本阶段**不会**创建 `/dev/gunyah`，也**不会**启动虚拟机。VM manager 和 DroidVM
接入属于后续阶段。

## 设备基线

| 项目 | 值 |
|---|---|
| 机型 | Redmi K60 `mondrian` (SM8475) |
| Android | 16 / SDK 36 |
| 内核 | `5.10.252-gki-g19f54b03cc87` |
| Gunyah | EL2 已运行，API 1，variant 81 |
| 当前 VMID | 3 |
| 虚拟化节点 | `/dev/gunyah`、`/dev/kvm`、`/dev/gzvm` 均不存在 |
| 内核配置 | `MODVERSIONS=y`、`MODULE_SIG=n`、LTO/CFI/SCS/PTR_AUTH 开启 |

## 隐私与数据边界

本项目把设备隐私安全作为硬约束：

- **不读取** `/data` 下用户目录、`/sdcard`、应用数据、通讯录、短信、账号、图片或文档。
- **不抓取** `logcat`、`dumpsys` 或完整 `dmesg`。
- 设备侧只读取系统层信息：`getprop`、`uname`、`/proc`、`/sys`、`/dev`、设备树和系统配置。
- 内核日志只按 `gunyah_probe` / `gunyah` / `hyp` 等虚拟化关键词过滤后查看。
- **不读取** `vm-data` 分区。
- 仓库不包含设备序列号、用户数据、厂商 `.ko`、固件或完整设备日志。
- Magisk 模块只在 `/data/adb/k60_gunyah_probe/` 写入自身的失败计数和状态标记。

更多说明见 [SECURITY.md](SECURITY.md)。

## 当前验证结果（2026-10-18）

- GitHub Actions 已成功构建 commit `7c3ed49`，CI 校验了目标 vermagic、GPL license、`__versions` 段和 Magisk zip。
- 有线 ADB 手动 `insmod` 成功，读到的结果为：
  - hypervisor identify：`api=1 variant=81`
  - `gh_rm_get_vmid(primary)=3`
  - `gh_rm_get_vmid(self)=0`（self 是 VMID 0 的别名）
  - `gh_rm_get_vminfo()` 返回 `0`，但 primary/self 的 guid、uri、name、sign_auth 均为空
  - `gh_rm_get_vm_id_info(3)` 返回 `-EINVAL (-22)`
- `rmmod gunyah_probe` 成功，模块无残留；SELinux 保持 `Enforcing`。
- Magisk zip 已通过 `magisk --install-module` 安装验证，Action 开关和 `service.sh` 受控加载路径均通过；发布默认保持自动加载关闭。

`gh_rm_get_vm_id_info(3) = -EINVAL` 说明当前 RM 对 primary VM 的标识查询并未成功返回；后续 VM manager 不能假设该查询可用，需要继续确认 RM 策略和可用 RPC。

## 构建

构建固定使用：

- 内核源码：`crdroidandroid/android_kernel_xiaomi_sm8450`
- commit：`38c6baf4bb72f783bb3588f9f1240603632a6c83`（5.10.252，含 SM8450 Gunyah 驱动）
- 目标 release：`5.10.252-gki-g19f54b03cc87`
- 工具链：LLVM/Clang 22（GitHub Actions 中从 `apt.llvm.org` 安装）

GitHub Actions 会生成：

- `gunyah_probe.ko`
- `k60-gunyah-probe-magisk.zip`

工作流位于 `.github/workflows/build.yml`。

`config/Module.symvers` 由设备上 391 个预编译模块的 ELF 元数据生成，包含 4201
个导入符号 CRC 和 2688 个导出符号 CRC。生成脚本只读取 ELF 符号表、`__versions`、
`__ksymtab` 和 `.modinfo`：

```bash
python3 scripts/gen_symvers.py \
  --mods /path/to/private/device-modules \
  --output config/Module.symvers \
  --vermagic "5.10.252-gki-g19f54b03cc87 SMP preempt mod_unload modversions aarch64"
```

厂商模块不应提交到本仓库。

## 手动验证（推荐先做）

先确认有线 ADB、SELinux 和 Root：

```powershell
adb devices -l
adb shell getenforce
adb shell su -c id
```

预期分别为设备为 `device`、`Enforcing`、`uid=0(root)`。

加载前确认虚拟化基线：

```powershell
adb shell su -c "uname -r"
adb shell su -c "cat /sys/hypervisor/type"
adb shell su -c "cat /sys/hypervisor/version/api"
adb shell su -c "cat /sys/hypervisor/version/variant"
adb shell su -c "ls -l /dev/gunyah /dev/kvm /dev/gzvm 2>&1"
```

推送并手动加载：

```powershell
adb push gunyah_probe.ko /data/local/tmp/gunyah_probe.ko
adb shell su -c "insmod /data/local/tmp/gunyah_probe.ko"
```

只查看本模块与 Gunyah 相关的过滤日志：

```powershell
adb shell su -c "dmesg | grep -iE 'gunyah_probe|gunyah:|hyp' | tail -n 80"
```

预期关键行包括：

```text
gunyah_probe: hypervisor identify: api=1 variant=81 ...
gunyah_probe: gh_rm_get_vmid(primary): vmid=3
gunyah_probe: probe complete ... safe to rmmod
```

卸载：

```powershell
adb shell su -c "rmmod gunyah_probe"
adb shell su -c "dmesg | grep -i gunyah_probe | tail -n 20"
```

如果加载失败，只保留与模块加载相关的过滤行，例如：

```text
version magic ... should be ...
disagrees about version of symbol ...
Unknown symbol ...
invalid ELF header / Exec format error
```

不要导出完整内核日志。

## Magisk 模块

`k60-gunyah-probe-magisk.zip` 可以在 Magisk Manager 中直接安装。安全设计：

- **默认不自动加载**；安装后只提供手动 `insmod` 使用。
- 在 Magisk Manager 中执行模块 Action，可切换“下一次开机自动加载”。
- 只有显式启用自动加载后，`service.sh` 才会执行 `insmod`。
- 加载前检查 `gh_rm_drv`、`gh_arm_drv`、`gh_msgq`、`gh_dbl` 已加载。
- 启动成功后等待 `sys.boot_completed=1`，才写入稳定标记。
- 连续启动失败达到阈值时，自动写入 `disable`，下一次开机不再加载。
- 标准 Magisk Safe Mode（开机时按住音量下）会禁用模块。
- 模块不挂载或替换任何系统文件，只在自身目录放置 `.ko` 和脚本。

### 恢复方法

紧急情况下任选其一：

1. 重启并进入 Magisk Safe Mode。
2. 在 Magisk Manager 中关闭模块。
3. 通过 ADB 删除模块目录：

```powershell
adb shell su -c "rm -rf /data/adb/modules/k60_gunyah_probe"
adb reboot
```

4. 卸载或禁用后确认 SELinux 恢复：

```powershell
adb shell getenforce
```

不要把 SELinux 长期保持在 Permissive。

## 仓库结构

```text
.github/workflows/build.yml   GitHub Actions 构建与 Release
config/config.gz              设备内核配置
config/Module.symvers         设备模块 ELF 元数据导出的 CRC
kernel/gunyah_probe.c         只读探测模块
kernel/Makefile               外部模块构建入口
magisk/                       Magisk 模块模板
scripts/gen_symvers.py         Module.symvers 生成器
scripts/build_module.sh       模块构建脚本
scripts/build_magisk.sh       Magisk zip 打包脚本
```

## 完整内核构建

设备真实布局、原始启动分区备份、boot v4 参数和完整 `Image` 构建流程见
[KERNEL_BUILD.md](KERNEL_BUILD.md)。

## 第二阶段可行性

`/dev/gunyah` VM manager 的可行性边界、crosvm UAPI 需求、现有厂商 API 缺口和可接受路线，
见 [PHASE2_ANALYSIS.md](PHASE2_ANALYSIS.md)。

当前结论：在不修改内核/boot 的约束下，仅靠可加载模块无法安全补齐完整 VM manager。

## 下一阶段

1. 已完成：`gunyah_probe.ko` 加载、日志和 `rmmod` 均验证通过。
2. 根据 RM 查询结果确认非 trusted VM 策略。
3. 回移最小 VM manager、vCPU 和内存接口。
4. 出现 `/dev/gunyah` 后，验证 crosvm/DroidVM UAPI 兼容性。
5. 最后再评估是否启用 Magisk 自动加载。

## 许可

GPL-2.0-only。Gunyah 头文件与内核接口来自对应 GPL 内核源码。
