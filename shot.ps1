# shot.ps1 — launch m2hle, capture window screenshots at given delays (seconds).
# Usage: shot.ps1 <romzip> <outprefix> <delay1> [delay2 ...]
param([string]$rom, [string]$prefix, [int[]]$delays)
$root = $PSScriptRoot
$exe = "$root\build\Release\m2hle.exe"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SShot { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); public struct R { public int L,T,Rg,B; } }
"@
Add-Type -AssemblyName System.Drawing
$p = Start-Process -FilePath $exe -ArgumentList "--rom","$rom","--run" -WorkingDirectory $root -PassThru
$prev = 0
foreach ($d in $delays) {
    Start-Sleep -Seconds ($d - $prev); $prev = $d
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq 0) { Write-Host "skip $d (no window handle)"; continue }
    [SShot]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
    $r = New-Object SShot+R; [SShot]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Rg - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { Write-Host "skip $d (bad rect)"; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
    $out = "$root\build\${prefix}_${d}s.png"
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    Write-Host "saved $out"
}
Stop-Process -Id $p.Id -Force
