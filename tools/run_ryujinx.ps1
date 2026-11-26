# katamari-nx: one emulator run of the NRO in the user's patched Ryubing with this port's own data folder
# (docs/porting-start-here.md of banjo-nx: "Build and test essentials"). Started minimised; refuses to start while any
# Ryujinx runs or when the free commit memory is under -MinCommitGB; stops only the process it started.
#   powershell -File tools/run_ryujinx.ps1 -Seconds 300 -Name emu1 [-Toml <a katamari.toml with test keys>]
# renut-ultimate (2026-11-07): its own data folder (ryu-data\renut-ultimate, made from katamari-nx's), and the NRO of
# app\out\<-Build> copied to its SD at each run (katamari-nx's folder and NRO are left alone).
param([int]$Seconds = 240, [string]$Name = "emu", [string]$Toml = "", [double]$MinCommitGB = 12, [string]$Build = "switch")
$root = Split-Path -Parent $PSScriptRoot
$ryu = "D:\New folder\Ryubing-src\ryubing\build\Ryujinx.exe"
$data = "D:\New folder\ryu-data\renut-ultimate"
# Ryujinx starts an NRO without argv: the app then takes REX_SWITCH_FALLBACK_EXE (app/CMakeLists.txt) as its own path.
$sd = Join-Path $data "sdcard\switch\katamari-nx"
$nro = Join-Path $sd "katamari-nx.nro"
$out = Join-Path $root "out\emu_runs\$Name"
if (Get-Process -Name Ryujinx -ErrorAction SilentlyContinue) { Write-Output "a Ryujinx is running (another port's?): not starting"; exit 2 }
$os = Get-CimInstance Win32_OperatingSystem
$freeCommit = ($os.TotalVirtualMemorySize - ($os.TotalVirtualMemorySize - $os.FreeVirtualMemory)) / 1MB
if ($freeCommit -lt $MinCommitGB) { Write-Output ("free commit memory {0:N1} GB < {1} GB: not starting" -f $freeCommit, $MinCommitGB); exit 3 }
$built = Join-Path $root "app\out\$Build\renut.nro"
if (-not (Test-Path $built)) { Write-Output "no NRO at $built"; exit 4 }
Copy-Item $built $nro -Force
# ilomilo-nx (2026-11-25; renut-ultimate port 291): the SD's own toml is put back after the run.
$sdToml = Join-Path $sd "katamari.toml"
$savedToml = Join-Path $root "out\emu_sd_toml_saved.toml"
if ($Toml) { if (Test-Path $sdToml) { Copy-Item $sdToml $savedToml -Force } else { Remove-Item $savedToml -ErrorAction SilentlyContinue }; Copy-Item $Toml $sdToml -Force }
# renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27): an NRO without the copies' guard forms (the everyday and
# release builds) calls every native copy directly whatever the toml's guard keys say: say so before the run.
$cache = Join-Path $root "app\out\$Build\CMakeCache.txt"
# renut-ultimate (2026-11-11, port 81; sasrt-nx's machine copies in place): an NRO with the copies in place (the
# everyday and release builds) reads no copy's key and runs no guard, no counting form and no A/B target 70.
$inPlace = (Test-Path $cache) -and (Select-String -Path $cache -Pattern '^SASRT_HOT_IN_PLACE:BOOL=(ON|1|TRUE|YES|Y)$' -Quiet)
if ($Toml -and $inPlace -and
    (Select-String -Path $Toml -Pattern ('^\s*(banjo_hot_guard_calls|banjo_hot_guard_period)\s*=\s*[1-9]|' +
        '^\s*(banjo_cpu_verify|banjo_hot_count)\s*=\s*true|^\s*banjo_cpu_native_\w+\s*=\s*false|' +
        '^\s*banjo_renut_ab_targets?\s*=.*\b70\b') -Quiet)) {
    Write-Output ("note: app\out\$Build has the copies in place (SASRT_HOT_IN_PLACE ON): the toml's copy keys, guard / " +
                  "verify keys, banjo_hot_count and A/B target 70 do nothing there (tools\build_switch_guard.bat, then " +
                  "-Build switch-guard)")
}
if ($Toml -and -not $inPlace -and (Test-Path $cache) -and
    (Select-String -Path $cache -Pattern '^SASRT_HOT_GUARD_FORMS:BOOL=(OFF|0|FALSE|NO|N)$' -Quiet) -and
    (Select-String -Path $Toml -Pattern '^\s*(banjo_hot_guard_calls|banjo_hot_guard_period)\s*=\s*[1-9]|^\s*banjo_cpu_verify\s*=\s*true' -Quiet)) {
    Write-Output ("note: app\out\$Build has no guard forms (SASRT_HOT_GUARD_FORMS OFF): the toml's guard / verify " +
                  "keys check no native copy (tools\build_switch_guard.bat, then -Build switch-guard)")
}
New-Item -ItemType Directory -Force $out | Out-Null
$logs = Join-Path $sd "logs"
if (Test-Path $logs) { Remove-Item -Recurse -Force $logs }
# ilomilo-nx (2026-11-25, its handoff 4; renut-ultimate port 291): in Ryujinx the NRO has no usable argv[0], so the
# rexglue log folder (logs/rex next to the NRO, switch_crash_hooks.c) is never set and those logs (the stall
# watchdog's and CPU sampler's banjo_profiler.log, rex_profile.log, rex_stderr.log, a crash's rex_crash.log; the
# watchdog's trigger file stall_dump) are written in the working folder, the SD's /switch/: cleared before the
# run, collected into the run's folder after.
$swRoot = Split-Path -Parent $sd
$rexLogs = @("banjo_profiler.log", "rex_profile.log", "rex_stderr.log", "rex_crash.log", "stall_dump")
foreach ($f in $rexLogs) { $p0 = Join-Path $swRoot $f; if (Test-Path $p0) { Remove-Item -Force $p0 } }
$env:DOTNET_ROOT = "D:\New folder\dotnet10"
# katamari-nx (2026-10-31): the run's own start time ($p.StartTime filtered nothing: every run copied every capture
# the SD had ever kept, ~800 MB a run, 19 GB in out/emu_runs).
$started = Get-Date
$p = Start-Process -FilePath $ryu -ArgumentList @("-r", "`"$data`"", "`"$nro`"") -PassThru -WindowStyle Minimized `
     -RedirectStandardOutput (Join-Path $out "ryujinx_stdout.txt") -RedirectStandardError (Join-Path $out "ryujinx_stderr.txt")
Write-Output ("started Ryujinx pid {0} (free commit {1:N1} GB)" -f $p.Id, $freeCommit)
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 3 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id) after $Seconds s" }
else { Write-Output "Ryujinx exited by itself (code $($p.ExitCode))" }
Start-Sleep -Seconds 2
if (Test-Path $logs) { Copy-Item -Recurse $logs (Join-Path $out "logs") -Force }
foreach ($f in $rexLogs) { $p0 = Join-Path $swRoot $f; if (Test-Path $p0) { Copy-Item $p0 (Join-Path $out $f) -Force } }
if ($Toml) { if (Test-Path $savedToml) { Copy-Item $savedToml $sdToml -Force; Remove-Item $savedToml } else { Remove-Item $sdToml -ErrorAction SilentlyContinue }; Write-Output "the SD's toml put back" }
# This run's captures move out of the SD (the SD's folder no longer grows); older ones there are left alone.
Get-ChildItem $sd -Filter "*.png" -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started } |
    Move-Item -Destination $out -Force
Get-ChildItem $out -Recurse | Select-Object FullName, Length
