# ilomilo-nx (2026-11-25): on-demand stall dumps during an emulator run (the stall watchdog's file trigger, port 153
# and 167: a file named stall_dump in the NRO's log folder dumps every guest thread's wait and guest call chain to
# banjo_profiler.log). Started next to tools/run_ryujinx.ps1 with a toml that has banjo_stall_watch_seconds > 0; it
# writes the trigger into this port's emulator SD folder (never the console's) at the given seconds.
#   powershell -File tools/ryujinx_stall_dumps.ps1 -At "45,90,150"   (seconds from this script's start)
# In Ryujinx the trigger is sdmc:/switch/stall_dump (the NRO's rexglue log folder is not set there: banjo_profiler.log's
# second line names the trigger); on the console it would be <NRO folder>/logs/rex/stall_dump.
param([string]$At = "45,90,150", [string]$Switch = "D:\New folder\ryu-data\renut-ultimate\sdcard\switch")
$start = Get-Date
foreach ($s in ($At.Split(",") | ForEach-Object { [int]$_ } | Sort-Object)) {
    $wait = $s - ((Get-Date) - $start).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds $wait }
    Set-Content -Path (Join-Path $Switch "stall_dump") -Value "" -Encoding ascii
    Write-Output ("stall_dump at {0} s" -f $s)
}
