# K60 内核与 boot 镜像构建说明

生成时间：2026-10-18

## 1. 设备真实启动布局

Redmi K60 `mondrian` 当前为 A/B 设备，启动相关分区为：

```text
boot_a        192 MiB
boot_b        192 MiB
vendor_boot_a  96 MiB
vendor_boot_b  96 MiB
dtbo_a         24 MiB
dtbo_b         24 MiB
vbmeta_a      128 KiB
vbmeta_b      128 KiB
```

设备**没有 `init_boot_a` / `init_boot_b` 分区**。因此本项目不能使用带独立
`init_boot` 的新 GKI 流程；必须构建并替换 `boot.img` 中的 kernel，同时保持当前
`vendor_boot` 和 ramdisk 配比不变。

## 2. Boot 镜像格式

设备当前 `boot_a`：

```text
magic: ANDROID!
header version: 4
page size: 4096
kernel size: 46874924
ramdisk size: 2885584
OS version: 16.0.0
OS patch level: 2026-04
cmdline: empty
signature size: 0
```

`vendor_boot_a` 同样为 header v4，包含：

```text
vendor_ramdisk00: 3244955 bytes
vendor_ramdisk01: 9695138 bytes
dtb: 6687382 bytes
bootconfig: 101 bytes
```

自定义内核只替换 `boot` 中的 kernel，保留原始 ramdisk。`vendor_boot`、`dtbo` 和
`vbmeta` 不修改。

## 3. 原始分区备份

本地备份目录：

```text
C:\Users\userasus\Desktop\test\k60-stock-backup\k60_stock_backup
```

文件 SHA-256：

```text
boot_a.img          81c10917cd0283bd8e1e39efea5b9a1caa648dae267e72fca67d4f1c425cb4f9
boot_b.img          854ffb85b66226b6ebcddbb8691ed2466c721d87bf4907001f774cdca119a6cf
vendor_boot_a.img   8cda2998270010069b80a1532509aeda9b57bbfb43d702f9820da85f92fb9f1d
vendor_boot_b.img   50e4a9ba23791f5db3ee661c6787244036b29074b659bb092dc3a48786ed8585
dtbo_a.img          97e56d6c22f7ed9b8b2a0fad3a8ccf7ee43339ee12c768832442c18d90dd2518
dtbo_b.img          97e56d6c22f7ed9b8b2a0fad3a8ccf7ee43339ee12c768832442c18d90dd2518
vbmeta_a.img        da3011f72453ab7519aed0d210eb1a16b6475dc636fd9ab1df74aa2557673075
vbmeta_b.img        2934c2a425fa07f8741247c64455062d06b8485d8a85b60dc502a1ee0de99393
```

备份镜像不提交到 Git，也不会上传到公开 Release。

## 4. 解锁状态

已通过 `fastboot getvar` 验证：

```text
unlocked: yes
current-slot: a
product: mondrian
```

Android 内 `ro.boot.flash.locked` 显示 `1` 与该设备当前 root/属性状态不一致，因此
以 fastboot 返回为准。

## 5. 构建流程

GitHub Actions 工作流：`.github/workflows/build-kernel.yml`

输入固定为：

```text
源码: crdroidandroid/android_kernel_xiaomi_sm8450
commit: 38c6baf4bb72f783bb3588f9f1240603632a6c83
release: 5.10.252-gki-g19f54b03cc87
config: 设备 /proc/config.gz 备份
```

CI runner 内存约束：设备原配置为 Full LTO，但 GitHub 标准 16 GB runner 在最终
`vmlinux.o` 链接阶段会被 OOM kill。工作流因此使用 Android 同样支持的 Thin LTO；
boot v4 参数、ramdisk、源码和 vermagic 均不变。符号 CRC 会在产物中另行对比。

构建产物：

```text
Image
System.map
kernel.config
Module.symvers
```

本地手工构建：

```bash
KERNEL_SRC=/path/to/kernel OUT_DIR=/path/to/out ./scripts/build_kernel.sh
```

## 6. 组装 boot.img

使用仓库脚本直接替换 `boot v4` 中的 kernel，并保留当前 `boot_a` 的 ramdisk：

```bash
python3 scripts/repack_boot_v4.py \
  --stock-boot /path/to/boot_a.img \
  --kernel /path/to/Image \
  --output boot-custom.img
```

已用原 kernel 做恒等重建测试，输出与原 `boot_a` 的有效 payload 完全一致：

```text
有效镜像长度: 49770496
SHA-256:      1f42ee215ee28f3e6cd49a48d56561f994ffcfb20a131cf7e257c713601b74c1
```

分区其余部分是零填充和 AVB footer。脚本默认输出未签名 payload；当前设备已通过
fastboot 验证 `unlocked: yes`。先用 `fastboot boot` 临时启动，不要先 `fastboot flash`。

### 6.1 临时启动验证

已使用原 kernel 重建的恒等 payload 执行：

```text
fastboot boot repacked_v4_identity.img
```

设备正常启动，结果：

```text
sys.boot_completed=1
uname -r = 5.10.252-gki-g19f54b03cc87
SELinux = Enforcing
```

该操作只从 RAM 临时启动，没有写入 `boot` 分区。说明当前解锁设备接受这种未签名
boot v4 payload，可以用于后续自定义内核的首轮测试。

## 7. 测试与回滚

测试顺序：

1. `fastboot boot boot-custom.img`
2. 检查是否正常启动、ADB、SELinux、/proc/version、vendor 模块加载。
3. 如果通过，再决定是否写入 `_b` 槽或当前槽。

回滚：

```powershell
fastboot flash boot_a .\k60-stock-backup\k60_stock_backup\boot_a.img
fastboot flash vendor_boot_a .\k60-stock-backup\k60_stock_backup\vendor_boot_a.img
fastboot flash dtbo_a .\k60-stock-backup\k60_stock_backup\dtbo_a.img
fastboot flash vbmeta_a .\k60-stock-backup\k60_stock_backup\vbmeta_a.img
fastboot reboot
```

任何刷写前都必须再次确认当前槽位和备份 SHA-256。

## 8. 隐私边界

内核构建只使用启动分区、系统配置、`/proc`、`/sys` 和模块元数据。没有读取或备份
`/sdcard`、用户 `/data` 目录、应用数据、通讯录、短信、图片、文档或 `vm-data`。
