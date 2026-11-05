# Verify the two A/B test packages. ASCII-only output on purpose.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($n in @("engine", "os")) {
    $zip = Join-Path $repo "dist/PBZ-Framework-Menu_0.5.2-$n.zip"
    $dir = Join-Path $repo "dist/_verify_$n"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    Expand-Archive -Path $zip -DestinationPath $dir
    $root = Join-Path $dir "PBZFM"
    Write-Host "--- $n ---"
    Get-ChildItem -Recurse -File $root | ForEach-Object {
        Write-Host ("  " + $_.FullName.Substring($root.Length + 1) + "  " + $_.Length)
    }
    $iniPath = Join-Path $root "PBZFM.ini"
    $ini = [System.IO.File]::ReadAllText($iniPath, [System.Text.Encoding]::UTF8)
    $lines = $ini -split "`r?`n"
    $modeLines = @($lines | Where-Object { $_ -match '^\s*input_mode\s*=' })
    Write-Host ("  ini bytes=" + (Get-Item $iniPath).Length + " lines=" + $lines.Count + " mode=[" + ($modeLines -join "; ") + "]")
    Write-Host ("  ini keeps CJK comments: " + $ini.Contains([char]0x8F93 + [string][char]0x5165))   # "in" + "put" chars from the header comment
    $dll = Join-Path $root "dlls/PBZFM.dll"
    Write-Host ("  dll bytes=" + (Get-Item $dll).Length + " sha256=" + (Get-FileHash $dll -Algorithm SHA256).Hash.Substring(0,16))
    $testdoc = Join-Path $root "TESTING_ENGINE_INPUT.md"
    Write-Host ("  test doc present: " + (Test-Path $testdoc))
}
