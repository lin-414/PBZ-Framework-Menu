# Pack the UE4SS mod zip: dist/PBZ-Framework-Menu_<version>.zip
# The version comes from CMakeLists.txt (project(PBZFM VERSION x.y.z)).
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$cm = Get-Content (Join-Path $repo "CMakeLists.txt") -Raw
if ($cm -notmatch 'project\(PBZFM\s+VERSION\s+([0-9][0-9.]*)') {
    throw "cannot read the project version from CMakeLists.txt"
}
$ver = $Matches[1]
# ABI gate: a missing export, an unexpected test hook or a stale baseline stops
# packaging here, before anything is written to dist/.
$abiCheck = Join-Path $repo "tools/check_abi_exports.ps1"
& $abiCheck -DllPath (Join-Path $repo "build/PBZFM.dll") -BaselinePath (Join-Path $repo "tests/abi/exports_baseline.txt")
if ($LASTEXITCODE -ne 0) {
    throw "ABI export check failed ($LASTEXITCODE); packaging aborted"
}
$stage = Join-Path $repo "dist/stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage/PBZFM/dlls" | Out-Null
New-Item -ItemType Directory -Force "$stage/PBZFM/languages" | Out-Null
New-Item -ItemType Directory -Force "$stage/PBZFM/sdk" | Out-Null
Copy-Item "$repo/build/PBZFM.dll" "$stage/PBZFM/dlls/"
New-Item -ItemType File -Path "$stage/PBZFM/enabled.txt" | Out-Null
Copy-Item "$repo/assets/PBZFM.ini" "$stage/PBZFM/"
Copy-Item "$repo/assets/languages/*.json" "$stage/PBZFM/languages/"
Copy-Item "$repo/assets/README.txt" "$stage/PBZFM/"
Copy-Item "$repo/sdk/pbzfm.h" "$stage/PBZFM/sdk/"
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
$cache = Join-Path $repo "build/CMakeCache.txt"
if ((Test-Path $cache) -and (Select-String -Path $cache -Pattern 'PBZFM_TEST_HOOKS:BOOL=ON' -Quiet)) {
    Write-Warning "build/ was configured with PBZFM_TEST_HOOKS=ON: the packaged DLL exports test hooks. Reconfigure with -DPBZFM_TEST_HOOKS=OFF and rebuild before shipping."
}
$zip = "$repo/dist/PBZ-Framework-Menu_$ver.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage/PBZFM" -DestinationPath $zip
Write-Host "packed: $zip (version $ver)"
