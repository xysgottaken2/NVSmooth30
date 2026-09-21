@echo off
setlocal
rem RTX 2060 / Turing smoke test.
rem Place this .bat, nvs30_probe.exe and the CI-built version.dll in ONE folder.
rem (Optional but recommended) run tools\inspect_nvp.py against your driver's
rem NvPresent64.dll first and note the "sm75:" verdict line.

set SM86_ENABLE_OSD=1
set SM86_DIAGNOSTICS=1
set SM86_ENABLE_D3D11_BRIDGE=1

rem --- Stage A: safe pass-through validation (never rewrites cubins) -------
nvs30_probe --frames 300
echo Stage A exit code: %errorlevel%  -- check nvsmooth30.log for:
echo   "cc=7.5" and "plan=TuringPolicy" and "GPU detect" lines.

rem --- Stage B (EXPERIMENTAL, may TDR the GPU): forced SM75 relabel -------
if /I "%~1"=="--force-experiment" (
  set SM75_FORCE_CUBIN_REWRITE=1
  nvs30_probe --frames 120
  echo Stage B exit code: %errorlevel%  -- record loader-rc from the log.
)
endlocal
