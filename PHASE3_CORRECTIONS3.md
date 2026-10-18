# 第三阶段补充修订 3：依赖墙与模块边界

生成时间：2026-10-18
关联：`PHASE3_CORRECTIONS2.md`

第二轮 CI（run `39112602700`）失败：

```text
Build arm64 Image with VM manager  completed failure
错误：CONFIG_GH_VM_MGR 不是 y：CONFIG_GH_VM_MGR=m
```

---

## 1. `GH_VM_MGR=y` 根本不可能

设备 `config/config.gz`：

```text
CONFIG_GUNYAH_DRIVERS=y
CONFIG_GH_RM_DRV=m        ← 关键
CONFIG_GH_CTRL=m
CONFIG_GH_MSGQ=m
CONFIG_GH_DBL=m
...
```

我的 Kconfig：

```text
config GH_VM_MGR
	tristate
	depends on GH_RM_DRV
```

`GH_RM_DRV=m` 时，依赖它的 tristate 只能取 `n` 或 `m`。
`scripts/config --enable GH_VM_MGR` 因此静默降级成 `=m`，
我的断言检查才发现。

要让它 `=y`，必须同时把 `GH_RM_DRV` 改成 `=y`，
也就是把厂商 RM 驱动编进内核、删掉 `gh_rm_drv.ko`——
而 DRM、qrtr、virtio 后端都 `depends on` 它。这是个大得多的改动。

**结论：VM manager 必须是独立模块。**

---

## 2. 这直接推翻了上一轮的方案

上一轮为了"不用导出符号"把 `GH_VM_MGR` 改成 `=y`。
现在 `=y` 走不通，`vm_mgr.c` 就不能直接调 `gh_rm_call()`
（`gh_rm_core.c` 里的模块内全局符号，无 `EXPORT_SYMBOL`）。

三条路：

| 方案 | 结论 |
|---|---|
| A 导出 `gh_rm_call()` | 可行，但把通用 RM 入口暴露给所有模块，ABI 面过大 |
| B `vm_mgr.o` 也塞进 `gh_rm_drv` | 可行，但 `gh_vm_mgr.o` 有 `module_init`（`misc_register`），失败会连带搞死厂商模块 |
| C `gh_vm_mgr.ko` 里自建 RM 客户端 | 死路，`gh_msgq_register()` 拒绝第二个 `GH_MSGQ_LABEL_RM` 客户端 |

**采用 A 的窄化版本**：

- `gh_rm_vmmgr.o` 进 `gh_rm_drv`（无 `module_init`，纯粹加代码，
  不改变 `gh_rm_drv` 的加载时机和成败）；
- `gh_vm_mgr.o` 保持独立模块；
- 只导出一个**专用**函数 `gh_rm_set_vm_boot_context()`，
  而不是通用的 `gh_rm_call()`。

`vm_mgr.c` 今天只需要这一个裸 RPC（`VM_SET_BOOT_CONTEXT`），
其余全部走已有的具名 API。导出一个名字明确的函数，
比导出通用入口安全得多。

---

## 3. 修掉的问题

- `Kconfig.fragment`：补上"不能 =y"的说明和原因；
- `Makefile.fragment`：改回 `obj-$(CONFIG_GH_VM_MGR)` 独立模块，
  修正注释；
- `gh_rm_vmmgr.c`：新增并导出 `gh_rm_set_vm_boot_context()`，
  线上格式（小端 + reg_set/reg_index 两个 u8）在函数内封装；
- `gh_rm_vmmgr.c`：补回被误删的文件头和 `#include`，
  并加上 `<uapi/linux/gunyah.h>`（`GH_VM_BOOT_CONTEXT_REG_SHIFT` 只在那边定义）；
- `vm_mgr.c`：改为调用 `gh_rm_set_vm_boot_context()`，
  删掉不再需要的 `gh_rm_drv_private.h` include；
- `build_kernel.sh`：`--module GH_VM_MGR`，加两条断言
  （`GH_VM_MGR=m` 且 `GH_RM_DRV=m` 未被改动），并开始构建 modules；
- CI verify 步骤同步改为检查 `=m` 和两个 `.ko`。

---

## 4. 副作用（需要知道）

`gh_rm_drv.ko` 现在由我们构建，不再用厂商那份。
内容上是"厂商原有 object + 我们的 `gh_rm_vmmgr.o`"，
厂商对象一个字节没改，但模块文件确实换了。

这意味着：

- 重新打包 boot.img 时，`vendor_dlkm` 里的 `gh_rm_drv.ko`
  需要用我们构建的版本覆盖，否则 `gh_rm_set_vm_boot_context`
  符号不存在，`gh_vm_mgr.ko` 加载失败；
- 或者把两个 `.ko` 一起交给 Magisk 模块，在厂商模块加载后 insmod。

这一点在 `PHASE3_STATUS.md` 的设备验证清单里必须列出来。
