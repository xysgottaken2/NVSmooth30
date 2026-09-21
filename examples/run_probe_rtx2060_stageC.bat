@echo off
setlocal
cd /d "%~dp0"
rem ============================================================
rem  NVSmooth30 - RTX 2060 STAGE C - ELF-stamp sweep
rem  Stage B measured loader-rc=300 (CUDA_ERROR_INVALID_SOURCE)
rem  with stamp=entryonly, which leaves the cubin ELF still
rem  declaring sm_89 (inconsistent pair). This sweep tries the
rem  two SELF-CONSISTENT sm_75 stamp words as well, so we can
rem  tell "container/ELF inconsistency" from "real ISA check".
rem  Same TDR caveat as Stage B (probe only, not ranked games).
rem ============================================================

if not exist version.dll (
  echo ERRO: coloque este .bat na mesma pasta do version.dll e do nvs30_probe.exe
  pause
  exit /b 1
)

if exist nvsmooth30.log move /y nvsmooth30.log nvsmooth30.prev.log >nul

set SM86_DIAGNOSTICS=1
set SM86_ENABLE_D3D11_BRIDGE=1
set SM86_ENABLE_OSD=1
set SM75_FORCE_CUBIN_REWRITE=1

echo ============ Mode 0: entryonly (baseline - Stage B mediu rc=300) ============
set SM75_ELF_STAMP=entryonly
nvs30_probe --frames 60
echo mode entryonly probe exit: %errorlevel%

echo ============ Mode 1: driver75 - ELF e_flags := 0x05004B04 ============
set SM75_ELF_STAMP=driver75
nvs30_probe --frames 60
echo mode driver75 probe exit: %errorlevel%

echo ============ Mode 2: mirror86 - ELF e_flags := 0x06004B04 ============
set SM75_ELF_STAMP=mirror86
nvs30_probe --frames 60
echo mode mirror86 probe exit: %errorlevel%

echo.
echo ============ RESULTADOS (todas as sessoes do log) ============
powershell -NoProfile -Command "Get-Content .\nvsmooth30.log | Select-String -Pattern 'session ','elf-stamp','EXPERIMENTAL','loader-rc','Refusing' | ForEach-Object { $_.Line }"

echo.
echo LEITURA: para CADA modo procure a linha EXPERIMENTAL ... loader-rc=N
echo  - todos os modos rc=300 ... libcuda valida a ISA; barreira final CONFIRMADA (documentada, zero risco)
echo  - algum modo rc=0       ... loader aceitou o par consistente; decisao de teste em jogo fica explicita
pause
endlocal
