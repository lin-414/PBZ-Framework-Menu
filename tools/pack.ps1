# Pack the UE4SS mod zip: dist/PBZ-Framework-Menu_<version><suffix>.zip
# The version comes from CMakeLists.txt (project(PBZFM VERSION x.y.z)).
#
# Default arguments produce the normal release package (build/ + no suffix).
# The scheme-C engine build uses:
#   tools\pack.ps1 -BuildDir build_engine -Suffix -engine -IniInputMode engine
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files that
# have no BOM as ANSI, so non-ASCII comments get mis-parsed and can silently
# swallow the next statement (that already cost us one empty PBZFM.ini).
param(
    [string]$BuildDir = "build",
    [string]$Suffix = "",
    # Overrides input_mode in the packaged PBZFM.ini (auto/engine/os).
    # Empty = ship assets/PBZFM.ini unchanged.
    [string]$IniInputMode = "",
    # Extra files copied into the mod folder (absolute, or relative to the repo).
    [string[]]$ExtraFiles = @()
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cm = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cm -notmatch 'project\(PBZFM\s+VERSION\s+([0-9][0-9.]*)') {
    throw "cannot read the project version from CMakeLists.txt"
}
$ver = $Matches[1]
if (-not (Test-Path (Join-Path $repo "$BuildDir/PBZFM.dll"))) {
    throw "PBZFM.dll not found under $BuildDir/ - build that configuration first"
}
# ABI gate: a missing export, an unexpected test hook or a stale baseline stops
# packaging here, before anything is written to dist/.
$abiCheck = Join-Path $repo "tools/check_abi_exports.ps1"
& $abiCheck -DllPath (Join-Path $repo "$BuildDir/PBZFM.dll") -BaselinePath (Join-Path $repo "tests/abi/exports_baseline.txt")
if ($LASTEXITCODE -ne 0) {
    throw "ABI export check failed ($LASTEXITCODE); packaging aborted"
}
$stage = Join-Path $repo "dist/stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage/PBZFM/dlls" | Out-Null
New-Item -ItemType Directory -Force "$stage/PBZFM/languages" | Out-Null
New-Item -ItemType Directory -Force "$stage/PBZFM/sdk" | Out-Null
Copy-Item "$repo/$BuildDir/PBZFM.dll" "$stage/PBZFM/dlls/"
New-Item -ItemType File -Path "$stage/PBZFM/enabled.txt" | Out-Null
Copy-Item "$repo/assets/PBZFM.ini" "$stage/PBZFM/"
if ($IniInputMode -ne "") {
    $iniPath = "$stage/PBZFM/PBZFM.ini"
    # Line-based rewrite with an explicit UTF8 read/write, then assert the result
    # is still a full ini. Never ship a truncated config.
    $lines = @(Get-Content -LiteralPath $iniPath -Encoding UTF8)
    if ($lines.Count -lt 5) { throw "staged PBZFM.ini has only $($lines.Count) lines - copy broken" }
    $found = $false
    $out = foreach ($line in $lines) {
        if ($line -match '^[ \t]*input_mode[ \t]*=') {
            $found = $true
            "input_mode = $IniInputMode"
        } else {
            $line
        }
    }
    if (-not $found) { $out = @("input_mode = $IniInputMode") + @($out) }
    Set-Content -LiteralPath $iniPath -Value $out -Encoding UTF8
    $size = (Get-Item $iniPath).Length
    if ($size -lt 200) { throw "packaged ini looks empty after pinning input_mode ($size bytes)" }
    Write-Host "input_mode pinned to '$IniInputMode' in the packaged ini ($size bytes)"
}
Copy-Item "$repo/assets/languages/*.json" "$stage/PBZFM/languages/"
Copy-Item "$repo/assets/README.txt" "$stage/PBZFM/"
Copy-Item "$repo/sdk/pbzfm.h" "$stage/PBZFM/sdk/"
foreach ($f in $ExtraFiles) {
    $src = if ([System.IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $repo $f }
    if (-not (Test-Path $src)) { throw "extra file not found: $src" }
    Copy-Item $src "$stage/PBZFM/"
}
# M9: licence/notice files and user theme overrides belong in the package - the
# README points at them and "themes\<name>.json" is a documented feature.
Copy-Item "$repo/LICENSE" "$stage/PBZFM/"
Copy-Item "$repo/THIRD_PARTY_NOTICES.md" "$stage/PBZFM/"
if (Test-Path "$repo/themes") {
    New-Item -ItemType Directory -Force "$stage/PBZFM/themes" | Out-Null
    Copy-Item "$repo/themes/*.json" "$stage/PBZFM/themes/"
}
# M8: a DLL built with PBZFM_TEST_HOOKS=ON carries the automation/injection
# exports; never ship one silently.
$cache = Join-Path $repo "$BuildDir/CMakeCache.txt"
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern 'PBZFM_TEST_HOOKS:BOOL=ON' -Quiet)) {
    Write-Warning "$BuildDir/ was configured with PBZFM_TEST_HOOKS=ON: the packaged DLL exports test hooks. Reconfigure with -DPBZFM_TEST_HOOKS=OFF and rebuild before shipping."
}
$zip = "$repo/dist/PBZ-Framework-Menu_$ver$Suffix.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage/PBZFM" -DestinationPath $zip
Write-Host "packed: $zip (version $ver, build dir $BuildDir)"
