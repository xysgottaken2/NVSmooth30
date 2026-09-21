@echo off
rem NVSmooth30 - ABI-PROBE survey (path 3 milestone 0)
rem Replaces the intercepted NvPresent CUDA modules with driver-JIT sm_75 no-op
rem stubs and logs every kernel contract (function names, grids, graph nodes,
rem allocations). No frame generation. Safe on hardware by construction.
setlocal
cd /d "%~dp0"
echo === NVSmooth30 ABI-PROBE run ===
echo env: SM75_ABI_PROBE=%SM75_ABI_PROBE% (this bat sets it)
echo.
set "SM75_ABI_PROBE=1"
if exist nvsmooth30.log move /y nvsmooth30.log nvsmooth30.prev.log >nul
nvs30_probe.exe
echo.
echo ===================== [nvs30-abi] contract log =====================
findstr /c:"nvs30-abi" nvsmooth30.log
echo =====================================================================
echo.
echo Cole aqui (ou no PR) as linhas [nvs30-abi] e [nvs30-turing] ABI-PROBE.
pause
endlocal
