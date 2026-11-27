# ssasr-nx (2026-11-25, from sasrt-nx's tools/win/lldb_run.ps1): the PC build under lldb (sasrt-nx's LLVM copy,
# D:\sonic transformed\_tools\llvm, read only) with a breakpoint on a function (default: the dispatcher's
# InvalidFunctionTrap, the "Call to invalid or unregistered function" stop): at the first hit it writes the stopped
# thread's backtrace (the generated functions are named sub_XXXXXXXX in the PDB) and stops the process.
#   powershell -File tools\lldb_trap.ps1 [-Seconds 120] [-Out out\lldb_trap.txt] [-Break InvalidFunctionTrap] [-Extra "--input_script=..."]
param([int]$Seconds = 120, [string]$Out = "out\lldb_trap.txt", [string]$Break = "InvalidFunctionTrap", [string]$Extra = "",
      [string]$Game = "D:/beautifulkatamari/game")
$root = Split-Path -Parent $PSScriptRoot
$lldb = "D:\sonic transformed\_tools\llvm\bin\lldb.exe"
$dir = Join-Path $root "app\out\pc"
$outPath = Join-Path $root $Out
$cmds = Join-Path $root "out\lldb_trap_cmds.txt"
$runArgs = @("--log_file=$root\out\lldb_trap.log", "--audio_mute=true", "--window_start_minimized=false", "--fullscreen=false",
          "--game_data_root=`"$Game`"", "--user_data_root=$root/out/pc_user", "--log_file_writer=false") +
        @($Extra -split ' ' | Where-Object { $_ })
$runLine = "run " + ($runArgs -join ' ')
@("breakpoint set -r $Break", $runLine, "thread backtrace -c 60", "image list -o -f renut.exe", "process kill", "quit") |
    Set-Content -Encoding ascii $cmds
$p = Start-Process -FilePath $lldb -WorkingDirectory $dir -PassThru -WindowStyle Minimized `
    -RedirectStandardOutput $outPath -RedirectStandardError ($outPath + ".err") `
    -ArgumentList @("-b", "-s", "`"$cmds`"", "--", "renut.exe")
if (-not $p.WaitForExit($Seconds * 1000)) {
    Get-CimInstance Win32_Process -Filter "Name='renut.exe'" | Where-Object { $_.ParentProcessId -eq $p.Id } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    "no stop in $Seconds s"
}
"lldb output: $outPath"
