@echo off
setlocal
cd /d "%~dp0"
rem ============================================================
rem  NVSmooth30 - RTX 2060 STAGE B (EXPERIMENTAL)
rem  Forces the sm_89 -> sm_75 fatbin relabel and measures what
rem  libcuda does with it (clean rejection vs acceptance).
rem  THIS CAN BLINK/RECOVER THE SCREEN (TDR). Save your work.
rem  Double-click safe: no command-line arguments needed.
rem ============================================================

if not exist version.dll (
  echo ERRO: coloque este .bat na mesma pasta do version.dll e do nvs30_probe.exe
  pause
  exit /b 1
)

rem Start a clean log for this run; keep the previous one for comparison.
if exist nvsmooth30.log move /y nvsmooth30.log nvsmooth30.prev.log >nul

set SM86_ENABLE_OSD=1
set SM86_DIAGNOSTICS=1
set SM86_ENABLE_D3D11_BRIDGE=1
set SM75_FORCE_CUBIN_REWRITE=1

echo ============ STAGE B - forced relabel, loader-rc measurement ============
echo (env check) SM75_FORCE_CUBIN_REWRITE=%SM75_FORCE_CUBIN_REWRITE%
echo The run is only meaningful if the log line says "forced-cubin-rewrite=1".
echo.
nvs30_probe --frames 120
echo probe exit code: %errorlevel%

echo.
echo ============ nvsmooth30.log - resultado do experimento ============
powershell -NoProfile -Command "Get-Content .\nvsmooth30.log | Select-String -Pattern 'forced-cubin-rewrite','EXPERIMENTAL','loader-rc','Refusing','Turing' | ForEach-Object { $_.Line } | Select-Object -First 30"

echo.
echo INTERPRETACAO:
echo  - loader-rc != 0  ... libcuda rejeitou o cubin rotulado sm_75 (rejeicao limpa, sem risco)
echo  - loader-rc = 0 e janela rodou ... aceiteito; proximo passo = teste em jogo com cuidado
echo  - loader-rc = 0 + tela piscou/TDR ... confirmado: executa nao decodifica em Turing
echo Cole o trecho acima (ou o nvsmooth30.log) no PR #1 para o proximo passo.
pause
endlocal
