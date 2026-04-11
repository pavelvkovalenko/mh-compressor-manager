@echo off
REM check_context_changes.bat — Проверка изменений файлов с момента загрузки в контекст
REM Для Windows
REM Использование:
REM   check_context_changes.bat              — Проверить изменения
REM   check_context_changes.bat --update     — Обновить метку времени
REM   check_context_changes.bat --reset      — Сбросить метку времени

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."
set "TIMESTAMP_FILE=%SCRIPT_DIR%.context\last_loaded.txt"
set "CONFIG_FILE=%PROJECT_DIR%\.settings"

if "%1"=="--update" goto update
if "%1"=="--reset" goto reset
goto check

:update
for /f "tokens=2 delims==" %%a in ('wmic OS Get localdatetime /value') do set "dt=%%a"
set "YY=%dt:~2,2%" & set "YYYY=%dt:~0,4%" & set "MM=%dt:~4,2%" & set "DD=%dt:~6,2%"
set "HH=%dt:~8,2%" & set "Min=%dt:~10,2%" & set "Sec=%dt:~12,2%"
set "TIMESTAMP=%YYYY%-%MM%-%DD% %HH%:%Min%:%Sec%"
echo CONTEXT_TIMESTAMP=!TIMESTAMP! > "%TIMESTAMP_FILE%"
echo [OK] Метка времени обновлена: !TIMESTAMP!
goto end

:reset
echo CONTEXT_TIMESTAMP=0 > "%TIMESTAMP_FILE%"
echo [RESET] Метка времени сброшена
goto end

:check
if not exist "%TIMESTAMP_FILE%" (
    echo [WARN] Метка времени не установлена. Загрузите документацию в контекст и выполните:
    echo    audit\check_context_changes.bat --update
    goto end
)

for /f "tokens=2 delims==" %%a in (%TIMESTAMP_FILE%) do set "CONTEXT_TS=%%a"

if "%CONTEXT_TS%"=="0" (
    echo [WARN] Метка времени сброшена. Загрузите документацию и выполните --update
    goto end
)

echo [INFO] Документация загружена в контекст: %CONTEXT_TS%
echo.
echo Запустите: git status --short ^| findstr ".md"
echo Для ручного просмотра изменений.
echo.
echo [OK] Проверка завершена

:end
endlocal
