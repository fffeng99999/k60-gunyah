# 安全与隐私说明

## 数据范围

本项目只处理系统层虚拟化信息。开发、构建、测试过程中执行以下约束：

- 不读取 `/sdcard`、`/storage` 或 `/data` 下的用户目录。
- 不读取应用数据、通讯录、短信、通话记录、账号、密码、图片或文档。
- 不读取 `vm-data` 分区。
- 不运行 `logcat`、`dumpsys`、`bugreport` 或完整 `dmesg` 导出。
- 不把设备序列号、固件、厂商 `.ko` 或完整设备日志写入仓库。

允许的设备侧读取范围：

- `getprop` 中的硬件/系统属性；
- `uname`；
- `/proc/device-tree/hypervisor` 等虚拟化设备树信息；
- `/sys/hypervisor`、`/sys/module` 和 `/proc/modules`；
- `/dev` 中的虚拟化节点状态；
- 经 `gunyah_probe`、`gunyah`、`hyp` 等关键词过滤后的内核日志。

## 模块行为

`gunyah_probe.ko` 只执行：

- Gunyah hypervisor identify hypercall；
- `gh_rm_get_vmid()` 查询；
- `gh_rm_get_vm_id_info()` 和 `gh_rm_get_vminfo()` 查询；
- `printk` 输出。

它不申请 VMID、不创建 VM、不映射/共享内存、不注册设备、不修改 hypervisor 状态。

## Magisk 状态文件

模块可能创建：

```text
/data/adb/k60_gunyah_probe/fail_count
/data/adb/k60_gunyah_probe/boot_ok
/data/adb/k60_gunyah_probe/service.log
```

这些文件只包含模块自身的启动状态，不包含用户内容。卸载脚本会删除该目录。

## 报告问题

提交 issue 时只附最小化、过滤后的系统信息。不要上传完整日志、设备序列号或任何用户数据。
