#!/usr/bin/env pwsh
# Regenerate kernel-intree/gh_vendor_decls.h from the vendor reference tree.
#
# The vendor headers under tools/src/crdroid are gitignored, but the
# pre-compile check needs to see the vendor declarations to tell a real typo
# from a symbol it simply cannot resolve. This extracts just the function
# names into a committed data file so the check works in CI as well.
#
# Run from the repo root after updating the vendor reference tree:
#   pwsh scripts/gen_vendor_decls.ps1

$ErrorActionPreference = "Stop"
$Root      = Split-Path -Parent $PSScriptRoot
$VendorDir = Join-Path (Split-Path -Parent $Root) "tools\src\crdroid"
$Out       = Join-Path $Root "kernel-intree\gh_vendor_decls.h"

if (-not (Test-Path $VendorDir)) {
    Write-Error "vendor reference tree not found at $VendorDir"
    exit 2
}

# Preprocessor objects that look like declarations but are not functions.
$notFunctions = @("ERR_PTR","IS_ERR","IS_ERR_OR_NULL","PTR_ERR","ERR_CAST","int","void","char","long","unsigned","signed","short","struct","union","enum","typedef","const","static","extern","register","volatile","inline","restrict","u8","u16","u32","u64","s8","s16","s32","s64","bool","size_t","__u8","__u16","__u32","__u64","__le16","__le32","__le64","__be16","__be32","__be64","gh_vmid_t","gh_label_t","gh_capid_t","gh_virq_handle_t","gh_memparcel_handle_t","gh_rm_msgid_t","gh_dbl_flags_t","FILE","va_list")

$names = New-Object System.Collections.ArrayList
foreach ($h in (Get-ChildItem "$VendorDir\include\*.h") + @(Get-Item "$VendorDir\gh_rm_drv_private.h")) {
    $txt = Get-Content $h.FullName -Raw
    [regex]::Matches($txt, "(?m)^\s*(?:extern\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*;") | ForEach-Object {
        $n = $_.Groups[1].Value
        if ($notFunctions -contains $n) { return }
        if ($names -notcontains $n) { [void]$names.Add($n) }
    }
}

$names.Sort()
$body = ($names | ForEach-Object { "int $(0)(void);" -f $_ }) -join "`n"

$dq = [string][char]34
$L = New-Object System.Collections.Generic.List[string]
$L.Add("/*")
$L.Add(" * Vendor Gunyah symbol declarations needed by the pre-compile check.")
$L.Add(" *")
$L.Add(" * The real vendor headers live in tools/src/crdroid, which is gitignored")
$L.Add(" * because it is reference material rather than project source. The check")
$L.Add(" * script needs to see the vendor declarations to tell a genuine typo from")
$L.Add(" * a symbol it simply cannot resolve, so the declarations it uses are")
$L.Add(" * extracted here.")
$L.Add(" *")
$L.Add(" * Regenerate with:  pwsh scripts/gen_vendor_decls.ps1")
$L.Add(" *")
$L.Add(" * This file is data for the checker only. It is never compiled.")
$L.Add(" */")
$L.Add("")
$L.Add("#ifndef _GH_VENDOR_DECLS_H")
$L.Add("#define _GH_VENDOR_DECLS_H")
$L.Add("")
$L.Add("/* ---- include/linux/gunyah/*.h and gh_rm_drv_private.h ---- */")
$L.Add("")
foreach ($n in $names) { $L.Add("int $n(void);") }
$L.Add("")
$L.Add("#endif")

# Also record every struct/enum/union TAG the vendor headers declare.
# The kernel has one tag namespace, so a UAPI type named gh_vm_status
# collides with the vendor struct gh_vm_status and the build fails with
# "use of gh_vm_status with tag type that does not match". That happened
# once; this list is what stops it recurring.
$tags = New-Object System.Collections.ArrayList
foreach ($h in (Get-ChildItem "$VendorDir\include\*.h") + @(Get-Item "$VendorDir\gh_rm_drv_private.h")) {
    $txt = Get-Content $h.FullName -Raw
    [regex]::Matches($txt, "(?m)^\s*(struct|enum|union)\s+(\w+)\s*\{") | ForEach-Object {
        $t = "$($_.Groups[1].Value) $($_.Groups[2].Value)"
        if ($tags -notcontains $t) { [void]$tags.Add($t) }
    }
}
$tags.Sort()
foreach ($t in $tags) { $L.Add("/* vendor-tag: $t */") }
Set-Content -Path $Out -Value ($L -join "`n") -Encoding utf8
Write-Output "wrote $Out with $($names.Count) declarations and $($tags.Count) vendor tags"
