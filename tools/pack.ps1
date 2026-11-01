# Pack the UE4SS mod zip: dist/PBZFM_<version>.zip
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
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
$zip = "$repo/dist/PBZ-Framework-Menu_0.3.2.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage/PBZFM" -DestinationPath $zip
Write-Host "packed: $zip"