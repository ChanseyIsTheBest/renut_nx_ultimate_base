# ssasr-nx (2026-11-25): a PC run (tools/run_pc.ps1's arguments) that watches for a hang: once the game's 10 s report
# ("[renut] 10.0 s: N game frames") has been seen, a gap of -Gap seconds without the next one attaches lldb (sasrt-nx's
# LLVM copy, D:\sonic transformed\_tools\llvm, read only) to the running process and writes every thread's backtrace
# (the generated functions are sub_XXXXXXXX in the PDB) and renut.exe's load address, then stops the process.
#   powershell -File tools\hang_dump.ps1 -Seconds 330 -Name hang1 [-Gap 25] [-Extra "--input_script=..."] [-Visible]
param([int]$Seconds = 300, [string]$Name = "hang", [int]$Every = 15, [int]$Gap = 25, [string]$Extra = "", [switch]$Visible,
      [string]$Game = "D:/beautifulkatamari/game")
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "app\out\pc"
$exe = Join-Path $dir "renut.exe"
$lldb = "D:\sonic transformed\_tools\llvm\bin\lldb.exe"
$logs = Join-Path $root "out\pc_runs\$Name"
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$min = if ($Visible) { "false" } else { "true" }
$runArgs = @("--log_file=$log", "--audio_mute=true", "--window_start_minimized=$min", "--fullscreen=false",
          "--game_data_root=$Game", "--user_data_root=$root/out/pc_user",
          "--nfsmw_captura_cada_s=$Every", "--nfsmw_captura_max=60", "--log_file_writer=false") +
        @($Extra -split ' ' | Where-Object { $_ })
$runArgs = @($runArgs | ForEach-Object { if ($_ -match ' ' -and $_ -notmatch '^".*"$') { '"' + $_ + '"' } else { $_ } })
$style = if ($Visible) { "Normal" } else { "Minimized" }
$p = Start-Process -FilePath $exe -ArgumentList $runArgs -WorkingDirectory $dir -PassThru -WindowStyle $style `
     -RedirectStandardError (Join-Path $logs "stderr.txt") -RedirectStandardOutput (Join-Path $logs "stdout.txt")
Write-Output "started pid $($p.Id)"
$deadline = (Get-Date).AddSeconds($Seconds)
$lastReport = $null; $reports = 0; $hung = $false
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
  Start-Sleep -Seconds 2
  if (Test-Path $log) {
    $n = 0
    try {
      $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
      $sr = New-Object System.IO.StreamReader($fs)
      while ($null -ne ($line = $sr.ReadLine())) { if ($line -match '\[renut\] 10\.0 s: \d+ game frames') { $n++ } }
      $sr.Close()
    } catch { }
    if ($n -gt $reports) { $reports = $n; $lastReport = Get-Date }
  }
  if ($lastReport -and ((Get-Date) - $lastReport).TotalSeconds -gt $Gap) { $hung = $true; break }
}
if ($hung) {
  Write-Output ("no 10 s report for {0} s after {1} reports: attaching lldb" -f $Gap, $reports)
  $cmds = Join-Path $logs "lldb_cmds.txt"
  @("process attach --pid $($p.Id)", "thread list", "thread backtrace all -c 40", "image list -o -f renut.exe",
    "process detach", "quit") | Set-Content -Encoding ascii $cmds
  $out = Join-Path $logs "hang_backtraces.txt"
  $l = Start-Process -FilePath $lldb -PassThru -WindowStyle Minimized -RedirectStandardOutput $out `
       -RedirectStandardError ($out + ".err") -ArgumentList @("-b", "-s", "`"$cmds`"")
  if (-not $l.WaitForExit(240000)) { Stop-Process -Id $l.Id -Force -ErrorAction SilentlyContinue; Write-Output "lldb timed out" }
  Write-Output "backtraces: $out"
  # ssasr-nx (2026-11-25, hang2): a second attach for the threads running in renut.exe itself (not waiting in the OS):
  # their registers (the generated code keeps guest registers in host ones: a float loop's value is in an xmm
  # register), the locals the PDB names, and the code at the PC. This lldb has no Python, so the threads are picked by
  # their ids from the first attach's list.
  $busy = @()
  foreach ($line in [System.IO.File]::ReadLines($out)) {
    if ($line -match '^\s+\*?\s*thread #\d+: tid = (0x[0-9a-f]+), 0x[0-9a-f]+ renut\.exe') { $busy += $Matches[1] }
  }
  if ($busy.Count -gt 0) {
    $cmds2 = Join-Path $logs "lldb_cmds2.txt"
    $lines = @("process attach --pid $($p.Id)")
    foreach ($tid in $busy) {
      $lines += @("thread select -t $tid", "thread backtrace -c 12", "register read --all", "frame variable",
                  "disassemble --pc -b -c 40", "frame select 1", "frame variable")
    }
    $lines += @("image list -o -f renut.exe", "process detach", "quit")
    $lines | Set-Content -Encoding ascii $cmds2
    $out2 = Join-Path $logs "hang_busy_threads.txt"
    $l2 = Start-Process -FilePath $lldb -PassThru -WindowStyle Minimized -RedirectStandardOutput $out2 `
          -RedirectStandardError ($out2 + ".err") -ArgumentList @("-b", "-s", "`"$cmds2`"")
    if (-not $l2.WaitForExit(240000)) { Stop-Process -Id $l2.Id -Force -ErrorAction SilentlyContinue; Write-Output "lldb (busy threads) timed out" }
    Write-Output ("busy threads ({0}): {1}" -f ($busy -join ' '), $out2)
  }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id)" }
else { Write-Output "exited by itself with code $($p.ExitCode)" }
Start-Sleep -Seconds 1
Get-ChildItem $dir -Filter "*.png" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $p.StartTime } |
    Move-Item -Destination $logs -Force -ErrorAction SilentlyContinue
Get-ChildItem (Join-Path $dir "capturas") -Filter "*.png" -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $p.StartTime } | Move-Item -Destination $logs -Force -ErrorAction SilentlyContinue
Write-Output "reports seen: $reports; hung: $hung"
