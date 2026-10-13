# Runs the recompiled game for N seconds, screenshots its window, then stops it.
# Usage: powershell -File run_and_capture.ps1 [-Seconds 180] [-Shots 3]
param([int]$Seconds = 180, [int]$Shots = 3)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
[Environment]::CurrentDirectory = $PSScriptRoot
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags); }
"@
$exe = Join-Path $PSScriptRoot 'PS2Recomp\out\build\x64-Debug\ps2xRuntime\ps2EntryRunner.exe'
$p = Start-Process -FilePath $exe -ArgumentList 'SLUS_214.51' -RedirectStandardOutput run_out.log -RedirectStandardError run_err.log -PassThru -NoNewWindow
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'shots') | Out-Null
try {
$interval = [math]::Max(1, [int]($Seconds / $Shots))
for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Seconds $interval
    $p.Refresh()
    if ($p.HasExited) { "game exited early (code $($p.ExitCode))"; break }
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { "no window yet"; continue }
    $r = New-Object Win+RECT; [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][Win]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $PSScriptRoot ("shots\t{0:D3}s.png" -f ($i * $interval)); $bmp.Save($file); $bmp.Dispose()
    "saved $file"
}
} finally {
    # Always stop the game, even if a screenshot fails.
    $p.Refresh()
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
"done"
