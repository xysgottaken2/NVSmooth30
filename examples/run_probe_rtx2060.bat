@echo off
setlocal
cd /d "%~dp0"
rem ============================================================
rem  NVSmooth30 - RTX 2060 (Turing/SM75) smoke test
rem  Keep this .bat, nvs30_probe.exe AND version.dll in ONE folder.
rem  The log is NOT sent anywhere: it is a text file named
rem  nvsmooth30.log created right here, next to the exe.
rem ============================================================

if not exist version.dll (
  echo ERRO: version.dll NAO esta nesta pasta.
  echo O Windows Defender pode ter movido o arquivo para a quarentena.
  echo Re-salve o zip / adicione excecao na pasta. Sem o version.dll o
  echo probe roda sem NVSmooth30 e nenhum log aparece.
  pause
  exit /b 1
)
if not exist nvs30_probe.exe (
  echo ERRO: nvs30_probe.exe nao esta nesta pasta.
  pause
  exit /b 1
)

rem Start a clean log for this run; keep the previous one for comparison.
if exist nvsmooth30.log move /y nvsmooth30.log nvsmooth30.prev.log >nul

set SM86_ENABLE_OSD=1
set SM86_DIAGNOSTICS=1
set SM86_ENABLE_D3D11_BRIDGE=1

echo ============ Stage A - seguro: deteccao + hooks + decisao de fatbin ============
nvs30_probe --frames 300
echo probe exit code: %errorlevel%

if not exist nvsmooth30.log (
  echo.
  echo NAO EXISTE nvsmooth30.log aqui. Possiveis causas:
  echo  1^ version.dll nao foi carregada - confirme que o exe e o dll estao na MESMA pasta
  echo  2^ quarentena do Defender removeu o version.dll - confira o historico de protecao
  echo  3^ pasta protegida (Program Files/OneDrive com Controlled folder access) - mova tudo
  echo     para um caminho simples, ex.: C:\nvs30\
  echo  4^ se o zip for de um build ANTIGO, antes do fix do import de
  echo     version.dll: baixe o artifact de novo - o exe antigo nao
  echo     carregava o proxy e por isso nao gravava log nenhum
  pause
  exit /b 1
)

echo.
echo ============ nvsmooth30.log - linhas de resultado ============
powershell -NoProfile -Command "Get-Content .\nvsmooth30.log | Select-String -Pattern 'startup','GPU detect','Turing','Loaded NvPresent','Gate located','gate/config','Config structure','cuModuleLoadData','NVP_Init','CUDA fatbin','DXGI Present hooks','Smooth Motion','WARNING','failed','not found' | ForEach-Object { $_.Line } | Select-Object -Last 40"

echo.
echo Dica: pressione F11 na janela do jogo/probe para mostrar/esconder o OSD.

if /I "%~1"=="--force-experiment" (
  echo.
  echo ============ Stage B EXPERIMENTAL - pode piscar/recuperar a tela (TDR) ============
  set SM75_FORCE_CUBIN_REWRITE=1
  nvs30_probe --frames 120
  echo Stage B probe exit code: %errorlevel%
  powershell -NoProfile -Command "Get-Content .\nvsmooth30.log | Select-String -Pattern 'EXPERIMENTAL','loader-rc' | ForEach-Object { $_.Line } | Select-Object -Last 10"
)

echo.
echo Copie o conteudo de nvsmooth30.log e cole no PR / aqui para analise.
pause
endlocal
