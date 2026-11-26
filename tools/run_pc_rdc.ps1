# ilomilo-nx (2026-11-25): app/out/pc/renut.exe (the Vulkan back end) started under RenderDoc (renderdoccmd capture)
# with qrenderdoc running tools/rdc_trigger.py, which connects to it and takes captures -Times seconds after connecting
# (as tools/run_ryujinx_rdc.ps1 does for Ryujinx; the app's own trigger, banjo_renderdoc_game_s, reported its capture
# FAILED on the first try): the PC's picture as the reference for a Switch-only fault seen in a Ryujinx capture.
# The window is shown (Renut presents only with a swapchain). Stops only the processes it started (found by path).
#   powershell -File tools/run_pc_rdc.ps1 -Seconds 60 -Name pc_rdc1 -Times "25" [-Extra "--input_script=..."]
param([int]$Seconds = 60, [string]$Name = "pc_rdc", [string]$Times = "25", [string]$Extra = "",
      [string]$Game = "D:/beautifulkatamari/game")
$root = Split-Path -Parent $PSScriptRoot
$rdcDir = if ($env:RENDERDOC_DIR) { $env:RENDERDOC_DIR } else { "D:\Banjo kazooie port\tools\RenderDoc_1.46_64" }
$dir = Join-Path $root "app\out\pc"
$exe = Join-Path $dir "renut.exe"
$out = Join-Path $root "out\pc_runs\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$args = @("capture", "-d", $dir, "-c", (Join-Path $out "pc"), $exe,
          "--log_file=$log", "--audio_mute=true", "--window_start_minimized=false", "--fullscreen=false",
          "--game_data_root=$Game", "--user_data_root=$root/out/pc_user", "--renut_gpu_backend=vulkan",
          "--log_file_writer=false") + @($Extra -split ' ' | Where-Object { $_ })
$args = @($args | ForEach-Object { if ($_ -match ' ' -and $_ -notmatch '^".*"$') { '"' + $_ + '"' } else { $_ } })
$started = Get-Date
$p = Start-Process -FilePath (Join-Path $rdcDir "renderdoccmd.exe") -ArgumentList $args -WorkingDirectory $dir -PassThru `
     -RedirectStandardError (Join-Path $out "stderr.txt") -RedirectStandardOutput (Join-Path $out "stdout.txt")
$env:RDC_OUT = $out
$env:RDC_TIMES = $Times
$q = Start-Process -FilePath (Join-Path $rdcDir "qrenderdoc.exe") -ArgumentList @("--python", "`"$(Join-Path $root 'tools\rdc_trigger.py')`"") `
     -PassThru -WindowStyle Minimized
Write-Output ("started renderdoccmd pid {0} and qrenderdoc pid {1}" -f $p.Id, $q.Id)
Start-Sleep -Seconds 3
$game = @(Get-Process -Name renut -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe -and $_.StartTime -ge $started })
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and $game.Count -and -not $game[0].HasExited) { Start-Sleep -Seconds 2 }
foreach ($g in $game) { if (-not $g.HasExited) { Stop-Process -Id $g.Id -Force; Write-Output "stopped renut pid $($g.Id) after $Seconds s" } }
Start-Sleep -Seconds 3
foreach ($x in @($q, $p)) { if (-not $x.HasExited) { Stop-Process -Id $x.Id -Force -ErrorAction SilentlyContinue } }
Get-Content (Join-Path $out "trigger.txt") -ErrorAction SilentlyContinue
