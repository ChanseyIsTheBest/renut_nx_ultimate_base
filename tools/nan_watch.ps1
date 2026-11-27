# ssasr-nx (2026-11-25): who writes a NaN into a guest float. A PC run (tools/run_pc.ps1's arguments) with lldb attached
# at -AttachAt seconds: a hardware write watchpoint on each -Address (guest addresses; the PC maps guest memory at
# 0x1000000000), stopping only when the word written is a NaN or infinity (big-endian in guest memory: the first byte's
# low 7 bits and the second byte's top bit all set), then the writing thread's backtrace (the PDB names the generated
# functions sub_XXXXXXXX), its registers, and detach. For the NaN frequency ratio XAudio2's converter is given
# (ssasr_resample_nan_guard): the converter's requested ratio is at its resampler state + 60.
#   powershell -File tools/nan_watch.ps1 -Name nanw1 -Seconds 420 -AttachAt 230 -Address 0xBFB03B8C,0xBFB03A24 -Extra "--input_script=..."
param([int]$Seconds = 420, [string]$Name = "nanw", [int]$AttachAt = 230, [string]$Address = "", [string]$Extra = "",
      [int]$Every = 30)
$root = Split-Path -Parent $PSScriptRoot
$lldb = "D:\sonic transformed\_tools\llvm\bin\lldb.exe"
$dir = Join-Path $root "app\out\pc"
$exe = Join-Path $dir "renut.exe"
$logs = Join-Path $root "out\pc_runs\$Name"
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path $logs "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$runArgs = @("--log_file=$log", "--audio_mute=true", "--window_start_minimized=true", "--fullscreen=false",
             "--game_data_root=D:/beautifulkatamari/game", "--user_data_root=$root/out/pc_user",
             "--nfsmw_captura_cada_s=$Every", "--nfsmw_captura_max=60", "--log_file_writer=false") +
           @($Extra -split ' ' | Where-Object { $_ })
$runArgs = @($runArgs | ForEach-Object { if ($_ -match ' ' -and $_ -notmatch '^".*"$') { '"' + $_ + '"' } else { $_ } })
$p = Start-Process -FilePath $exe -ArgumentList $runArgs -WorkingDirectory $dir -PassThru -WindowStyle Minimized `
     -RedirectStandardError (Join-Path $logs "stderr.txt") -RedirectStandardOutput (Join-Path $logs "stdout.txt")
Write-Output "started pid $($p.Id)"
$deadline = (Get-Date).AddSeconds($Seconds)
Start-Sleep -Seconds $AttachAt
# -Address "guard": the voices' addresses change from run to run, so wait for this run's first resampler-guard lines
# and watch those voices' requested ratio (state + 60): the same voices take a NaN again within seconds.
if ($Address -eq "guard") {
  $states = @()
  while ((Get-Date) -lt $deadline -and -not $p.HasExited -and $states.Count -lt 2) {
    Start-Sleep -Seconds 1
    if (Test-Path $log) {
      $states = @(Select-String -Path $log -Pattern 'resampler guard.*voice state ([0-9A-F]{8})' -AllMatches |
                  ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -Unique -First 2)
    }
  }
  $Address = ($states | ForEach-Object { '0x{0:X8}' -f ([Convert]::ToUInt32($_, 16) + 60) }) -join ','
  Write-Output ("watching this run's voices: " + $Address)
}
if (-not $p.HasExited -and $Address) {
  $cmds = Join-Path $logs "lldb_cmds.txt"
  $lines = @("process attach --pid $($p.Id)")
  $i = 1
  foreach ($a in ($Address -split ',')) {
    $host_ = [UInt64]0x1000000000 + [UInt64]([Convert]::ToUInt32($a.Trim().Replace('0x', ''), 16))
    $h = '0x{0:X}' -f $host_
    $b1 = '0x{0:X}' -f ($host_ + 1)
    $lines += "watchpoint set expression -w write -s 4 -- $h"
    $lines += "watchpoint modify -c `"((*(unsigned char*)$h) & 0x7F) == 0x7F && ((*(unsigned char*)$b1) & 0x80) == 0x80`" $i"
    $i++
  }
  $lines += @("watchpoint list", "continue", "image list -o -f renut.exe", "thread backtrace -c 30", "register read",
              "watchpoint list", "process detach", "quit")
  $lines | Set-Content -Encoding ascii $cmds
  $out = Join-Path $logs "nan_watch.txt"
  $env:LLDB_USE_NATIVE_PDB_READER = "1"  # the PDB's names (sub_XXXXXXXX) in the backtrace
  $l = Start-Process -FilePath $lldb -PassThru -WindowStyle Minimized -RedirectStandardOutput $out `
       -RedirectStandardError ($out + ".err") -ArgumentList @("-b", "-s", "`"$cmds`"")
  $left = [int](($deadline - (Get-Date)).TotalMilliseconds)
  if (-not $l.WaitForExit([Math]::Max($left, 10000))) {
    Stop-Process -Id $l.Id -Force -ErrorAction SilentlyContinue; Write-Output "no NaN write caught before the deadline"
  }
  Write-Output "lldb: $out"
}
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id)" }
