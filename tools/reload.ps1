# Build ce-lighting and (re)load it into a running MCC: unload Spark (a fake F9, Spark's own dev
# trick), install the new DLL, inject Spark again. Mods load once a Halo CE level runs. If MCC isn't
# running, start it without anti-cheat first (Steam: "Play with anti-cheat disabled").
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam").SteamPath
$mods = Join-Path $steam "steamapps\common\Halo The Master Chief Collection\MCC\Binaries\Win64\mods"
$env:Path = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin;" + $env:Path

$build = & MSBuild.exe "$root\ce-lighting.sln" /t:ce-lighting /t:spark-launcher /p:Configuration=$Config /p:Platform=Win64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { $build | Select-String " error |error [A-Z]+[0-9]+"; exit 1 }

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class CeLightingKeys { [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra); }
'@
$p = Get-Process MCC-Win64-Shipping -ErrorAction SilentlyContinue
if ($p -and ($p.Modules | Where-Object ModuleName -eq 'spark.dll')) {
    [CeLightingKeys]::keybd_event(0x78, 0, 0, [UIntPtr]::Zero)  # F9: Spark un-injects itself
    Start-Sleep -Milliseconds 80
    [CeLightingKeys]::keybd_event(0x78, 0, 2, [UIntPtr]::Zero)
    $t = Get-Date
    do { Start-Sleep 1; $p.Refresh() } while (($p.Modules | Where-Object ModuleName -in 'spark.dll', 'ce-lighting.dll') -and ((Get-Date) - $t).TotalSeconds -lt 20)
}
New-Item -ItemType Directory -Force $mods | Out-Null
Copy-Item "$root\bin\$Config-Win64\ce-lighting\ce-lighting.dll" $mods -Force
if ($p) { & "$root\vendor\spark\bin\$Config-Win64\spark-launcher\spark-launcher.exe" }
Start-Sleep 6
Get-Content "$mods\ce-lighting.log" -ErrorAction SilentlyContinue
