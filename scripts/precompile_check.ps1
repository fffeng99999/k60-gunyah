#!/usr/bin/env pwsh
# Static pre-compile check for the K60 in-tree Gunyah VM manager.
#
# Purpose: catch the classes of error that cost a 25-minute CI round,
# before pushing. Checks:
#   1. calls to functions not declared in any header we include, not
#      defined in our sources, and not kernel-provided
#   2. struct member access on a struct that does not declare it
#   3. C90 declaration-after-statement
#   4. brace / paren / comment imbalance
#   5. exported symbols colliding with the vendor drivers
#
# Usage:  pwsh scripts/precompile_check.ps1
# Exit 0 = clean, 1 = findings.

param(
    [string]$KernelSrc = ""
)

$ErrorActionPreference = "Stop"

$Root      = Split-Path -Parent $PSScriptRoot
$SrcDir    = Join-Path $Root "kernel-intree"
$VendorDir = Join-Path (Split-Path -Parent $Root) "tools\src\crdroid"

$ourHdr  = Get-ChildItem "$SrcDir\*.h"
$ourSrc  = Get-ChildItem "$SrcDir\*.c"
$venHdr  = @()
if (Test-Path "$VendorDir\include") {
    $venHdr = Get-ChildItem "$VendorDir\include\*.h"
} else {
    Write-Output "   note: $VendorDir\include absent (gitignored); vendor-symbol checks reduced"
}

$venPriv  = Join-Path $VendorDir "gh_rm_drv_private.h"
$venSrc   = @()
if (Test-Path $VendorDir) {
    $venSrc = Get-ChildItem "$VendorDir\*.c"
}

# uabi_offsets.c is a host-side reference checker compiled against the C
# library, not by the kernel, so it is excluded from kernel-symbol checks.
$kernSrc = @($ourSrc | Where-Object { $_.Name -ne "uabi_offsets.c" })

$findings = New-Object System.Collections.ArrayList
function Add-Finding([string]$where, [string]$msg) {
    [void]$findings.Add([pscustomobject]@{ Where = $where; Msg = $msg })
}

$declared = @{}
function Register-Symbols([string]$text, [string]$origin) {
    [regex]::Matches($text, "\b([A-Za-z_]\w{1,})\b") | ForEach-Object {
        if (-not $declared.ContainsKey($_.Groups[1].Value)) { $declared[$_.Groups[1].Value] = $origin }
    }
}
foreach ($h in $ourHdr)  { Register-Symbols (Get-Content $h.FullName -Raw) ("our:" + $h.Name) }
foreach ($h in $venHdr)  { Register-Symbols (Get-Content $h.FullName -Raw) ("vendor:" + $h.Name) }
if (Test-Path $venPriv) { Register-Symbols (Get-Content $venPriv -Raw) "vendor:gh_rm_drv_private.h" }

$defined = @{}
foreach ($f in $kernSrc) {
    [regex]::Matches((Get-Content $f.FullName -Raw), "(?m)^(?:static\s+)?(?:int|void|long|bool|u\d+|s\d+|struct\s+[\w\s\*]+?)\s+(\w+)\s*\(") |
        ForEach-Object { $defined[$_.Groups[1].Value] = $f.Name }
}

$kernelList = @(
    "kmalloc",
    "kzalloc",
    "kcalloc",
    "kfree",
    "kvfree",
    "kmemdup",
    "krealloc",
    "alloc_page",
    "alloc_pages",
    "free_page",
    "__free_pages",
    "page_address",
    "get_page",
    "put_page",
    "mutex_init",
    "mutex_lock",
    "mutex_unlock",
    "mutex_lock_interruptible",
    "mutex_trylock",
    "spin_lock",
    "spin_unlock",
    "spin_lock_irq",
    "spin_unlock_irq",
    "spin_lock_init",
    "spin_lock_irqsave",
    "spin_unlock_irqrestore",
    "down_read",
    "down_write",
    "up_read",
    "up_write",
    "down_read_interruptible",
    "down_write_interruptible",
    "down_read_trylock",
    "down_write_trylock",
    "init_rwsem",
    "init_waitqueue_head",
    "wait_event",
    "wait_event_interruptible",
    "wait_event_timeout",
    "wait_for_completion",
    "wait_for_completion_timeout",
    "wait_for_completion_interruptible",
    "wait_for_completion_killable",
    "init_completion",
    "complete",
    "complete_all",
    "reinit_completion",
    "try_wait_for_completion",
    "request_irq",
    "request_threaded_irq",
    "free_irq",
    "enable_irq_wake",
    "disable_irq_wake",
    "schedule_work",
    "schedule_delayed_work",
    "flush_work",
    "cancel_work_sync",
    "cancel_work",
    "INIT_WORK",
    "INIT_DELAYED_WORK",
    "kref_init",
    "kref_get",
    "kref_put",
    "kref_get_unless_zero",
    "kref_read",
    "module_put",
    "try_module_get",
    "module_refcount",
    "list_add",
    "list_del",
    "list_add_tail",
    "list_add_rcu",
    "list_del_rcu",
    "list_move",
    "list_splice",
    "list_splice_init",
    "list_empty",
    "list_is_singular",
    "list_cut_position",
    "list_entry",
    "list_first_entry",
    "list_last_entry",
    "list_for_each_entry",
    "list_for_each_entry_safe",
    "list_for_each_entry_continue",
    "list_for_each_entry_reverse",
    "rb_insert_color",
    "rb_insert",
    "rb_erase",
    "rb_first",
    "rb_next",
    "rb_prev",
    "rb_last",
    "rb_find",
    "rb_find_add",
    "rb_replace_node",
    "rb_replace_node_rcu",
    "rb_link_node",
    "rb_next_postorder",
    "rb_first_postorder",
    "xa_load",
    "xa_store",
    "xa_erase",
    "xa_find",
    "xa_alloc",
    "xa_reserve",
    "xa_init",
    "xa_empty",
    "xa_destroy",
    "xa_err",
    "xa_is_err",
    "xa_for_each",
    "idr_alloc",
    "idr_alloc_cyclic",
    "idr_find",
    "idr_remove",
    "idr_init",
    "idr_destroy",
    "idr_preload",
    "idr_preload_end",
    "misc_register",
    "misc_deregister",
    "cdev_init",
    "cdev_add",
    "cdev_del",
    "cdev_alloc",
    "alloc_chrdev_region",
    "unregister_chrdev_region",
    "register_chrdev_region",
    "device_create",
    "device_destroy",
    "class_create",
    "class_destroy",
    "anon_inode_getfile",
    "anon_inode_getfd",
    "get_unused_fd_flags",
    "put_unused_fd",
    "fd_install",
    "fdget",
    "fdput",
    "pin_user_pages_fast",
    "unpin_user_pages",
    "unpin_user_page",
    "get_user_pages",
    "get_user_pages_fast",
    "pin_user_pages",
    "account_locked_vm",
    "copy_from_user",
    "copy_to_user",
    "clear_user",
    "copy_in_user",
    "u64_to_user_ptr",
    "mmgrab",
    "mmdrop",
    "get_task_mm",
    "mmput",
    "IS_ERR",
    "IS_ERR_OR_NULL",
    "PTR_ERR",
    "ERR_PTR",
    "ERR_CAST",
    "min",
    "max",
    "min_t",
    "max_t",
    "clamp",
    "clamp_t",
    "overflows_type",
    "cpu_to_le16",
    "le16_to_cpu",
    "cpu_to_le32",
    "le32_to_cpu",
    "cpu_to_le64",
    "le64_to_cpu",
    "cpu_to_le8",
    "le8_to_cpu",
    "cpu_to_be16",
    "be16_to_cpu",
    "cpu_to_be32",
    "be32_to_cpu",
    "cpu_to_be64",
    "be64_to_cpu",
    "lower_32_bits",
    "upper_32_bits",
    "lower_16_bits",
    "upper_16_bits",
    "page_to_phys",
    "page_to_pfn",
    "phys_to_page",
    "virt_to_page",
    "virt_to_phys",
    "pfn_to_page",
    "pr_err",
    "pr_warn",
    "pr_info",
    "pr_debug",
    "pr_err_ratelimited",
    "pr_warn_ratelimited",
    "pr_info_ratelimited",
    "pr_err_once",
    "pr_warn_once",
    "pr_cont",
    "dev_err",
    "dev_warn",
    "dev_info",
    "dev_dbg",
    "dev_err_ratelimited",
    "dev_warn_ratelimited",
    "dev_notice",
    "snprintf",
    "scnprintf",
    "sprintf",
    "vsnprintf",
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "memchr",
    "strcmp",
    "strlen",
    "strncmp",
    "strcpy",
    "strncpy",
    "strlcpy",
    "strscpy",
    "container_of",
    "ARRAY_SIZE",
    "typeof",
    "offsetof",
    "WARN_ON",
    "WARN_ON_ONCE",
    "BUG_ON",
    "BUILD_BUG_ON",
    "eventfd_ctx_fdget",
    "eventfd_ctx_fileget",
    "eventfd_ctx_put",
    "eventfd_ctx_remove_wait_queue",
    "eventfd_signal",
    "eventfd_ctx_read",
    "init_waitqueue_func_entry",
    "init_poll_funcptr",
    "add_wait_queue",
    "remove_wait_queue",
    "vfs_poll",
    "key_to_poll",
    "wake_up",
    "wake_up_all",
    "wake_up_interruptible",
    "wake_up_locked",
    "arm_smccc_1_1_hvc",
    "arm_smccc_1_2_hvc",
    "arm_smccc_smc",
    "printk",
    "printk_ratelimited",
    "request_module",
    "module_param",
    "module_param_named",
    "module_param_cb",
    "module_param_string",
    "bitmap_fill",
    "bitmap_zero",
    "bitmap_set",
    "msecs_to_jiffies",
    "jiffies_to_msecs",
    "ktime_get",
    "ns_to_timespec64",
    "kvfree",
    "vzalloc",
    "vmalloc",
    "vfree",
    "dma_alloc_coherent",
    "dma_free_coherent",
    "iounmap",
    "ioremap",
    "of_find_compatible_node",
    "of_find_node_by_path",
    "of_get_child_by_name",
    "of_get_parent",
    "of_node_put",
    "of_property_read_u32",
    "of_property_read_string",
    "of_match_device",
    "of_device_is_compatible",
    "kasprintf",
    "kvasprintf",
    "mod_timer",
    "timer_setup",
    "del_timer",
    "queue_work",
    "alloc_workqueue",
    "destroy_workqueue",
    "kmem_cache_create",
    "kmem_cache_destroy",
    "kmem_cache_alloc",
    "kmem_cache_free",
    "srcu_notifier_chain_register",
    "srcu_notifier_chain_unregister",
    "srcu_notifier_call_chain",
    "atomic_inc",
    "atomic_dec",
    "atomic_set",
    "atomic_read",
    "refcount_set",
    "refcount_inc",
    "refcount_dec",
    "print_hex_dump_debug",
    "hex_dump_to_buffer"
)
foreach ($s in $kernelList) { if (-not $declared.ContainsKey($s)) { $declared[$s] = "kernel" } }

# Not functions: keywords, preprocessor helpers, macros that look like calls.
$skipSet = @{}
$skipSet["if"] = $true
$skipSet["for"] = $true
$skipSet["while"] = $true
$skipSet["switch"] = $true
$skipSet["return"] = $true
$skipSet["sizeof"] = $true
$skipSet["typeof"] = $true
$skipSet["case"] = $true
$skipSet["do"] = $true
$skipSet["else"] = $true
$skipSet["goto"] = $true
$skipSet["break"] = $true
$skipSet["continue"] = $true
$skipSet["struct"] = $true
$skipSet["union"] = $true
$skipSet["enum"] = $true
$skipSet["typedef"] = $true
$skipSet["static"] = $true
$skipSet["const"] = $true
$skipSet["volatile"] = $true
$skipSet["register"] = $true
$skipSet["extern"] = $true
$skipSet["inline"] = $true
$skipSet["restrict"] = $true
$skipSet["defined"] = $true
$skipSet["__attribute__"] = $true
$skipSet["__asm__"] = $true
$skipSet["__asm"] = $true
$skipSet["__extension__"] = $true
$skipSet["_Static_assert"] = $true
$skipSet["static_assert"] = $true
$skipSet["__builtin"] = $true
$skipSet["__builtin_choose_expr"] = $true
$skipSet["__builtin_offsetof"] = $true
$skipSet["alignof"] = $true
$skipSet["_Alignof"] = $true
$skipSet["_Generic"] = $true
$skipSet["__label__"] = $true
$skipSet["_Noreturn"] = $true
$skipSet["__inline"] = $true
$skipSet["__always_inline"] = $true
$skipSet["__deprecated"] = $true
$skipSet["__packed"] = $true
$skipSet["__aligned"] = $true
$skipSet["assert"] = $true
$skipSet["panic"] = $true
$skipSet["pr_fmt"] = $true
$skipSet["EXPORT_SYMBOL"] = $true
$skipSet["EXPORT_SYMBOL_GPL"] = $true
$skipSet["MODULE_LICENSE"] = $true
$skipSet["MODULE_AUTHOR"] = $true
$skipSet["MODULE_DESCRIPTION"] = $true
$skipSet["MODULE_VERSION"] = $true
$skipSet["MODULE_ALIAS"] = $true
$skipSet["MODULE_DEVICE_TABLE"] = $true
$skipSet["__stringify"] = $true
$skipSet["stringify"] = $true
$skipSet["__chk"] = $true
$skipSet["module_init"] = $true
$skipSet["module_exit"] = $true
$skipSet["__init"] = $true
$skipSet["__exit"] = $true
$skipSet["__devinit"] = $true
$skipSet["__devexit"] = $true
$skipSet["notrace"] = $true
$skipSet["__must_check"] = $true
$skipSet["__acquires"] = $true
$skipSet["__releases"] = $true
$skipSet["__must_hold"] = $true
$skipSet["__bitwise"] = $true
$skipSet["__force"] = $true
$skipSet["__user"] = $true
$skipSet["__kernel"] = $true
$skipSet["IS_ENABLED"] = $true
$skipSet["__same_type"] = $true
$skipSet["__is_constexpr"] = $true
$skipSet["__cmp"] = $true
$skipSet["__careful_cmp"] = $true
$skipSet["__typesafe"] = $true
$skipSet["ALIGN"] = $true
$skipSet["IS_ALIGNED"] = $true
$skipSet["PTR_ALIGN"] = $true
$skipSet["IS_ERR"] = $true
$skipSet["IS_ERR_OR_NULL"] = $true
$skipSet["PTR_ERR"] = $true
$skipSet["ERR_PTR"] = $true
$skipSet["ERR_CAST"] = $true
$skipSet["BIT"] = $true
$skipSet["BIT_ULL"] = $true
$skipSet["GENMASK"] = $true
$skipSet["GENMASK_ULL"] = $true
$skipSet["FIELD_GET"] = $true
$skipSet["FIELD_PREP"] = $true
$skipSet["READ_ONCE"] = $true
$skipSet["WRITE_ONCE"] = $true
$skipSet["read_once"] = $true
$skipSet["write_once"] = $true
$skipSet["smp_store_release"] = $true
$skipSet["smp_load_acquire"] = $true
$skipSet["rcu_assign_pointer"] = $true
$skipSet["rcu_dereference"] = $true
$skipSet["rcu_read_lock"] = $true
$skipSet["rcu_read_unlock"] = $true
$skipSet["srcu_read_lock"] = $true
$skipSet["srcu_read_unlock"] = $true
$skipSet["srcu_dereference"] = $true
$skipSet["list_for_each_entry"] = $true
$skipSet["list_for_each_entry_safe"] = $true
$skipSet["list_for_each_entry_continue"] = $true
$skipSet["list_for_each_entry_rcu"] = $true
$skipSet["container_of"] = $true
$skipSet["ARRAY_SIZE"] = $true
$skipSet["typeof"] = $true
$skipSet["offsetof"] = $true
$skipSet["abs"] = $true
$skipSet["swap"] = $true
$skipSet["roundup"] = $true
$skipSet["rounddown"] = $true
$skipSet["DIV_ROUND_UP"] = $true
$skipSet["__always_unused"] = $true
$skipSet["likely"] = $true
$skipSet["unlikely"] = $true

Write-Output "== precompile static check =="
Write-Output "   symbols known: $($declared.Count)  defined-in-our-src: $($defined.Count)"

# Literal-stripping regexes, defined once so call sites stay readable.
$dq = [string][char]34
$sq = [string][char]39
$dquoteRe = $dq + "(?:\\.|[^" + $dq + "\\])*" + $dq
$squoteRe = $sq + "(?:\\.|[^" + $sq + "\\])*" + $sq
$emptyDq = $dq + $dq
$emptySq = $sq + $sq

# ---------------------------------------------------------- 1. bad calls
foreach ($f in $kernSrc) {
    $lines2 = [System.IO.File]::ReadAllLines($f.FullName)
    $raw    = Get-Content $f.FullName -Raw
    for ($i = 0; $i -lt $lines2.Count; $i++) {
        $l = $lines2[$i]
        if ($l -match "^\s*(//|\*|/\*)") { continue }
        $scan = [regex]::Replace($l, $dquoteRe, $emptyDq)
        $scan = [regex]::Replace($scan, $squoteRe, $emptySq)
        [regex]::Matches($scan, "(?<![\.\w>\-])([a-z_]\w{1,})\s*\(") | ForEach-Object {
            $fn = $_.Groups[1].Value
            if ($skipSet.ContainsKey($fn)) { return }
            if ($declared.ContainsKey($fn)) { return }
            if ($defined.ContainsKey($fn)) { return }
            if ($raw -match ("(?m)^\s*(?:static\s+)?[A-Za-z_][\w\s\*]*\b" + [regex]::Escape($fn) + "\s*\(")) { return }
            Add-Finding "$($f.Name):$($i+1)" "call to undeclared function $fn"
        }
    }
}

# --------------------------------------------------- 2. struct members
$structMembers = @{}
foreach ($h in @($ourHdr) + @($venHdr)) {
    $txt = Get-Content $h.FullName -Raw
    [regex]::Matches($txt, "(?s)struct\s+(\w+)\s*\{(.*?)\n\};") | ForEach-Object {
        $name = $_.Groups[1].Value
        $body = $_.Groups[2].Value
        if (-not $structMembers.ContainsKey($name)) { $structMembers[$name] = @{} }
        # plain "type name;" members
        [regex]::Matches($body, "(?m)^\s*[A-Za-z_][\w\s\*]*?\b(\w+)\s*(?:\[[^\]]*\])?\s*;") | ForEach-Object {
            $structMembers[$name][$_.Groups[1].Value] = $true
        }
        # function-pointer members: "long (*bind)(struct foo *);"
        [regex]::Matches($body, "\(\s*\*\s*(\w+)\s*\)\s*\(") | ForEach-Object {
            $structMembers[$name][$_.Groups[1].Value] = $true
        }
        # Anonymous unions, flattened two levels:
        #   union { struct { __u64 phys_addr; } mmio; }
        # must expose "mmio" AND the fields inside it.
        [regex]::Matches($body, "(?s)union\s*\{.*?\}") | ForEach-Object {
            $u = $_.Value
            [regex]::Matches($u, "\}\s*(\w+)\s*;") | ForEach-Object {
                $structMembers[$name][$_.Groups[1].Value] = $true
            }
            [regex]::Matches($u, "(?m)^\s*[A-Za-z_][\w\s\*]*?\b(\w+)\s*(?:\[[^\]]*\])?\s*;") | ForEach-Object {
                $structMembers[$name][$_.Groups[1].Value] = $true
            }
        }
        # Named members of any nested anonymous struct:
        #   struct { int a; } foo;   ->   exposes "foo"
        [regex]::Matches($body, "\}\s*(\w+)\s*;") | ForEach-Object {
            $structMembers[$name][$_.Groups[1].Value] = $true
        }
    }
}

foreach ($f in $kernSrc) {
    $lines2 = [System.IO.File]::ReadAllLines($f.FullName)

    # Record every "struct X *name" declaration with its line number, then
    # resolve each "name->member" against the NEAREST preceding
    # declaration. A variable name can be reused for a different struct in
    # another function, so a single binding per name is wrong.
    $decls = @()
    for ($i = 0; $i -lt $lines2.Count; $i++) {
        if ($lines2[$i] -match "^\s*(//|\*|/\*)") { continue }
        foreach ($m in [regex]::Matches($lines2[$i], "struct\s+(\w+)\s*\*\s*(\w+)\s*[;=,)]")) {
            $decls += [pscustomobject]@{ Line = $i; Type = $m.Groups[1].Value; Name = $m.Groups[2].Value }
        }
    }

    for ($i = 0; $i -lt $lines2.Count; $i++) {
        if ($lines2[$i] -match "^\s*(//|\*|/\*)") { continue }
        foreach ($u in [regex]::Matches($lines2[$i], "\b(\w+)\s*->\s*(\w+)")) {
            $vname = $u.Groups[1].Value
            $member = $u.Groups[2].Value
            $decl = $null
            foreach ($d in $decls) {
                if ($d.Name -eq $vname -and $d.Line -lt $i) { $decl = $d }
            }
            if (-not $decl) { continue }
            if (-not $structMembers.ContainsKey($decl.Type)) { continue }
            if (-not $structMembers[$decl.Type].ContainsKey($member)) {
                Add-Finding "$($f.Name):$($i+1)" "struct $($decl.Type) has no member $member (via $vname)"
            }
        }
    }
}
# ------------------------------------------- 3. declaration after stmt
foreach ($f in $kernSrc) {
    $lines2 = [System.IO.File]::ReadAllLines($f.FullName)
    for ($i = 1; $i -lt $lines2.Count; $i++) {
        $prev = $lines2[$i-1].Trim()
        $cur  = $lines2[$i]
        if ($cur -notmatch "^\t+(struct|int|void|u8|u16|u32|u64|s8|s16|s32|s64|bool|size_t|unsigned|long|char|const|enum|__le\d+|__u\d+|__be\d+|gh_\w+_t)\s+[\w\*]+\s*[;=]") { continue }
        if ($cur -match "for\s*\(") { continue }
        if ($prev -notmatch ";\s*$") { continue }
        if ($prev -match "^\s*(struct|int|void|u8|u16|u32|u64|bool|size_t|unsigned|long|char|const|enum|__le|__u|__be|gh_\w+_t)\b") { continue }
        if ($prev -match "^\s*[\}\)]") { continue }
        if ($prev -match "^\s*(if|for|while|else|switch|do|case|default|return|break|continue)\b") { continue }
        if ($prev -match "^\s*(struct|enum|union|typedef)\b") { continue }
        if ($prev -match "^\s*$") { continue }
        if ($prev -match "^#") { continue }
        if ($prev -match "^\s*\{\s*$") { continue }
        Add-Finding "$($f.Name):$($i+1)" "declaration after statement: $prev then $($cur.Trim())"
    }
}

# ------------------------------------------------------- 4. balance
foreach ($f in @($kernSrc) + @($ourHdr)) {
    $t = Get-Content $f.FullName -Raw
    $ob = ([regex]::Matches($t, "\{")).Count
    $cb = ([regex]::Matches($t, "\}")).Count
    $op = ([regex]::Matches($t, "\(")).Count
    $cp = ([regex]::Matches($t, "\)")).Count
    $noComments = [regex]::Replace($t, "(?s)/\*.*?\*/", "")
    $oc = ([regex]::Matches($noComments, "/\*")).Count
    $cc = ([regex]::Matches($noComments, "\*/")).Count
    if ($ob -ne $cb) { Add-Finding $f.Name "brace imbalance $ob open / $cb close" }
    if ($op -ne $cp) { Add-Finding $f.Name "paren imbalance $op open / $cp close" }
    if ($oc -ne $cc) { Add-Finding $f.Name "comment imbalance $oc open / $cc close" }
}

# ------------------------------------------- 5. duplicate exports
# The vendor .c files are gitignored, so this check is skipped in CI. It
# still runs locally, which is where a symbol clash would be introduced.
$vendorExports = @{}
if (Test-Path $VendorDir) {
    Get-ChildItem "$VendorDir\*.c" | ForEach-Object {
        [regex]::Matches((Get-Content $_.FullName -Raw), "EXPORT_SYMBOL(?:_GPL)?\s*\(\s*(\w+)\s*\)") |
            ForEach-Object { $vendorExports[$_.Groups[1].Value] = $_.Name }
    }
} else {
    Write-Output "   note: vendor sources absent; duplicate-export check skipped"
}
foreach ($f in $kernSrc) {
    [regex]::Matches((Get-Content $f.FullName -Raw), "EXPORT_SYMBOL(?:_GPL)?\s*\(\s*(\w+)\s*\)") | ForEach-Object {
        $n = $_.Groups[1].Value
        if ($vendorExports.ContainsKey($n)) {
            Add-Finding $f.Name "export $n collides with vendor $($vendorExports[$n])"
        }
    }
}

# ------------------------------------------- 5b. tag collisions
#
# C has a single tag namespace, so "struct gh_vm_status" here and
# "enum gh_vm_status" in a UAPI header collide even though the kinds
# differ. That exact clash failed a build: the vendor tree has
# "struct gh_vm_status" and the Gunyah UAPI used to define
# "enum gh_vm_status". Compare tag NAMES, not kinds.
$vendorTags = @{}
$declsFile = Join-Path $SrcDir "gh_vendor_decls.h"
if (Test-Path $declsFile) {
    Select-String -Path $declsFile -Pattern "vendor-tag: (?:struct|enum|union) (\w+)" | ForEach-Object {
        $vendorTags[$_.Matches[0].Groups[1].Value] = $true
    }
}
foreach ($h in $ourHdr) {
    $txt = Get-Content $h.FullName -Raw
    [regex]::Matches($txt, "(?m)^\s*(?:struct|enum|union)\s+(\w+)\s*\{") | ForEach-Object {
        $tag = $_.Groups[1].Value
        if ($vendorTags.ContainsKey($tag)) {
            Add-Finding $h.Name "tag $tag collides with a vendor tag"
        }
    }
}

# ------------------------------------------- 6. kernel 5.10 API surface
#
# Everything below was verified by hand against the exact 5.10.252 tree
# after several of them turned out not to exist (rb_find, rb_find_add,
# overflows_type, enum gh_rm_vm_status, arm_smccc_1_2_hvc). Pass
# -KernelSrc to check them again against the real source tree; this is
# what the CI does, since it has the tree but not the vendor reference
# headers.
$apiChecks = @(
    @("xa_err",                          "include/linux/xarray.h"),
    @("xa_is_err",                       "include/linux/xarray.h"),
    @("xa_erase",                        "include/linux/xarray.h"),
    @("xa_store",                        "include/linux/xarray.h"),
    @("xa_load",                         "include/linux/xarray.h"),
    @("account_locked_vm",               "include/linux/mm.h"),
    @("pin_user_pages_fast",             "include/linux/mm.h"),
    @("unpin_user_pages",                "include/linux/mm.h"),
    @("FOLL_LONGTERM",                   "include/linux/mm.h"),
    @("kref_get_unless_zero",            "include/linux/kref.h"),
    @("compat_ptr_ioctl",                "include/linux/fs.h"),
    @("noop_llseek",                     "include/linux/fs.h"),
    @("anon_inode_getfile",              "include/linux/anon_inodes.h"),
    @("get_unused_fd_flags",             "include/linux/file.h"),
    @("init_poll_funcptr",               "include/linux/poll.h"),
    @("key_to_poll",                     "include/linux/wait.h"),
    @("poll_to_key",                     "include/linux/wait.h"),
    @("init_waitqueue_func_entry",       "include/linux/wait.h"),
    @("enable_irq_wake",                 "include/linux/interrupt.h"),
    @("IRQF_TRIGGER_RISING",             "include/linux/interrupt.h"),
    @("mmgrab",                          "include/linux/sched/mm.h"),
    @("mmdrop",                          "include/linux/sched/mm.h"),
    @("u64_to_user_ptr",                 "include/linux/kernel.h"),
    @("struct miscdevice",               "include/linux/miscdevice.h"),
    @("misc_register",                   "include/linux/miscdevice.h"),
    @("vm_fault_t",                      "include/linux/mm_types.h"),
    @("struct vm_fault",                 "include/linux/mm_types.h"),
    @("eventfd_ctx_fileget",             "include/linux/eventfd.h"),
    @("eventfd_ctx_remove_wait_queue",   "include/linux/eventfd.h"),
    @("min_t",                           "include/linux/minmax.h"),
    @("_gh_hcall",                       "arch/arm64/include/asm/gunyah/hcall.h"),
    @("struct gh_hcall_resp",            "include/linux/gunyah/hcall_common.h"),
    @("gh_hcall_dbl_send",               "drivers/virt/gunyah/hcall_dbl.h"),
    @("gh_hcall_dbl_mask",               "drivers/virt/gunyah/hcall_dbl.h"),
    @("wait_event_timeout",              "include/linux/wait.h"),
    @("msecs_to_jiffies",                "include/linux/jiffies.h"),
    @("IRQ_TYPE_EDGE_RISING",            "include/linux/irq.h"),
    @("of_find_compatible_node",         "include/linux/of.h"),
    @("of_property_read_u32",            "include/linux/of.h"),
    @("of_node_put",                     "include/linux/of.h"),
    @("complete",                        "include/linux/completion.h"),
    @("init_completion",                 "include/linux/completion.h"),
    @("schedule_work",                   "include/linux/workqueue.h"),
    @("INIT_WORK",                       "include/linux/workqueue.h"),
    @("request_irq",                     "include/linux/interrupt.h"),
    @("free_irq",                        "include/linux/interrupt.h"),
    @("eventfd_signal",                  "include/linux/eventfd.h"),
    @("kref_init",                       "include/linux/kref.h"),
    @("kref_put",                        "include/linux/kref.h"),
    @("xa_init",                          "include/linux/xarray.h")
)

if ($KernelSrc -ne "") {
    if (-not (Test-Path (Join-Path $KernelSrc "Makefile"))) {
        Add-Finding "kernel-src" "-KernelSrc is not a kernel tree: $KernelSrc"
    } else {
        foreach ($c in $apiChecks) {
            $sym = $c[0]; $rel = $c[1]
            $full = Join-Path $KernelSrc $rel
            if (-not (Test-Path $full)) {
                Add-Finding "5.10-api" "no $rel (needed for $sym)"
                continue
            }
            if (-not (Select-String -Path $full -SimpleMatch -Pattern $sym -Quiet)) {
                Add-Finding "5.10-api" "$sym missing from $rel"
            }
        }
        Write-Output "   kernel API surface checked against $KernelSrc"
    }
} else {
    Write-Output "   note: -KernelSrc not given; 5.10 API surface check skipped"
}

# ----------------------------------------------------------- report
Write-Output ""
if ($findings.Count -eq 0) {
    Write-Output "RESULT: clean - no findings"
    exit 0
}
Write-Output "RESULT: $($findings.Count) finding(s)"
$findings | Group-Object Where | Sort-Object Name | ForEach-Object {
    Write-Output ""
    Write-Output "--- $($_.Name) ---"
    $_.Group | ForEach-Object { Write-Output ("  {0}" -f $_.Msg) }
}
exit 1
