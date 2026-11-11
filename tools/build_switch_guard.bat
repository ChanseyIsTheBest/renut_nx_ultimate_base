@echo off
rem renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27, its build_switch.ps1 -GuardForms): a Switch NRO WITH the
rem localized copies' guard forms (SASRT_HOT_GUARD_FORMS ON), in a folder of its own. The everyday and release NROs
rem have none (the option's Switch default): there banjo_cpu_verify and banjo_hot_guard_* check no native copy (the
rem log says so). This one is for a guard or verify run on Ryujinx or the console, e.g.
rem   powershell -File tools\run_ryujinx.ps1 -Build switch-guard -Toml TOML
rem   with a TOML that sets banjo_hot_guard_calls = 4000 and banjo_hot_guard_period = 64 (the dense guard).
rem The everyday build's options otherwise (no LTO, no PGO). Output: app\out\switch-guard\renut.nro
setlocal
set "BUILD_DIR=%~dp0..\app\out\switch-guard"
set "CMAKE_EXTRA=-DSASRT_HOT_GUARD_FORMS=ON %CMAKE_EXTRA%"
set "GUARD_FORMS=ON"
call "%~dp0build_switch.bat" %*
