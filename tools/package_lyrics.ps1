# Packs the standalone karaoke lyrics ASI: FlatOut2-Lyrics-<version>.zip, laid out like the game folder.
# Works in flat-screen FlatOut 2 on its own and alongside FO2VR.asi in VR.
#   powershell -File tools\package_lyrics.ps1 -Version 1.0.0 [-AsiLoader <winmm.dll>] [-Out <folder>]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$AsiLoader,
    [string]$Out
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$game = Split-Path $repo
if (-not $AsiLoader) { $AsiLoader = Join-Path $game "winmm.dll" }
if (-not (Test-Path $AsiLoader)) {
    throw "Ultimate ASI Loader (winmm.dll) not found at '$AsiLoader'. Pass -AsiLoader <path-to-32bit-winmm.dll>."
}
if (-not $Out) { $Out = Join-Path $repo "build\release" }
$stage = Join-Path $Out "FlatOut2-Lyrics-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\scripts\FO2Lyrics-licenses" | Out-Null

Copy-Item $AsiLoader "$stage\winmm.dll"
Copy-Item "$repo\build\FO2Lyrics.asi" "$stage\scripts\"
Copy-Item "$repo\install\FO2Lyrics.ini" "$stage\scripts\"
Copy-Item "$repo\install\FO2Lyrics-README.md" "$stage\scripts\"
Copy-Item "$repo\tools\fetch_lyrics.ps1" "$stage\scripts\"
Copy-Item "$repo\tools\preview_lyrics.py" "$stage\scripts\"
Copy-Item "$repo\tools\preview_lyrics.cmd" "$stage\scripts\"
Copy-Item "$repo\tools\lyrics_preview.html" "$stage\scripts\"
Set-Content -Path "$stage\scripts\fetch_lyrics.cmd" -Encoding ASCII -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0fetch_lyrics.ps1`" %*`r`nif errorlevel 1 pause`r`n"
Copy-Item "$repo\LICENSE" "$stage\scripts\FO2Lyrics-licenses\FO2VR.txt"
Copy-Item "$repo\install\licenses\Ultimate-ASI-Loader.txt" "$stage\scripts\FO2Lyrics-licenses\"

$zip = Join-Path $Out "FlatOut2-Lyrics-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
"-> $zip"
