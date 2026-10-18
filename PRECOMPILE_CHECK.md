# 编译前静态检查

生成时间：2026-10-18

## 为什么加这个

一次完整内核 CI 约 25 分钟。前三轮 CI 暴露的问题里，至少两轮是
**本地花几秒就能发现的**：

| 轮次 | 失败原因 | 本地可否提前发现 |
|---|---|---|
| 2 | `CONFIG_GH_VM_MGR` 断言失败 | 可以，读一眼设备 config 即可 |
| 3 | `make Image` + `make modules` 分割调用导致 trace 头错误 | 难，属于内核构建特性 |
| 1 | 验证步骤自身逻辑错误 | 可以 |

真正拖时间的是**编译错误**。所以写了
`scripts/precompile_check.ps1`，在推送前跑一遍。

## 检查什么

| # | 检查项 | 能抓到的典型错误 |
|---|---|---|
| 1 | 调用未声明的函数 | 函数名拼错、漏 include、调了别的模块才有的符号 |
| 2 | 结构体没有该成员 | 成员名拼错、用了别处定义的 struct、union 嵌套层级搞错 |
| 3 | C90 声明后置语句 | 内核默认 `-Wdeclaration-after-statement`，是 error |
| 4 | 括号 / 注释不平衡 | 编辑时误删 |
| 5 | 导出符号与厂商冲突 | 重复定义 |

## 用法

```powershell
cd k60-gunyah
pwsh -NoProfile -File scripts\precompile_check.ps1
```

退出码 0 = 干净，1 = 有发现。已接入 CI，作为最前面的 gate。

## 符号表怎么来的

- `kernel-intree/*.h`：我们的头文件
- `tools/src/crdroid/include/*.h` + `gh_rm_drv_private.h`：厂商头
- `kernel-intree/*.c` 里有函数体的：同模块内可见
- 约 300 个 `linux/*.h` 提供的符号：硬编码清单
- C 关键字 / 宏：排除清单

清单不完整时会漏报，但**不会误报**（已验证，见下）。

## 验证过检查器本身有效

故意注入四类错误，确认都能抓到：

```text
call to undeclared function gh_rm_vm_init_TYPO
struct gh_vm has no member dtb_config_TYPO
declaration after statement: ... then int zz_after_stmt_test;
```

注入后报错，移除后恢复 clean。

## 已知局限

1. **只查语法和符号，不查语义。** 比如把 `GH_RM_RPC_MSG_ID_CALL_VM_INIT`
   写成另一个合法但错误的数字，查不出来。那类问题靠
   `PHASE3_CORRECTIONS2.md` 里那条"每次改动都与上游 v17 mbox 核对"。
2. **`uabi_offsets.c` 被排除**在符号检查外——它是主机侧的参考校验程序，
   用 C 标准库而不是内核头。
3. **厂商头里没有的 6.6 结构体**（`gh_rm_mem_parcel` 等）由
   `gh_vm_mgr_compat.h` 提供，检查器认识它们，但不会验证它们的字段是否
   和厂商 descriptor 真的对得上。这个只能靠读代码。
