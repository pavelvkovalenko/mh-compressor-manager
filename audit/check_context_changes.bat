@echo off
REM check_context_changes.bat — Проверка изменений файлов с момента загрузки в контекст
REM Для Windows
REM
REM Метка времени ХРАНИТСЯ В КОНТЕКСТЕ БЕСЕДЫ, а не в файле.
REM Передаётся как аргумент командной строки.
REM
REM Использование:
REM   audit\check_context_changes.bat <timestamp>     — Проверить изменения
REM   audit\check_context_changes.bat --list-files    — Список файлов с датами
REM
REM Аргумент <timestamp> — Unix timestamp из контекста беседы.
REM Получить текущий: powershell -Command "[int][double]::Parse((Get-Date -UFormat %%s))"

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%.."

if "%1"=="--list-files" goto list
if "%1"=="" goto no_args

echo [OK] Проверка изменений с метки времени: %1
echo Запустите: git status --short ^| findstr ".md"
echo Для просмотра изменённых файлов документации.
goto end

:list
echo Список отслеживаемых файлов:
echo   docs/specification/TECHNICAL_SPECIFICATION.md
echo   docs/development/QWEN.md
echo   docs/development/RULES.md
echo   docs/development/DEPLOY.md
echo   CONTRIBUTING.md
echo   README.md
echo   audit/CHANGELOG.md
echo   audit/ROUND_HISTORY.md
echo   audit/AUDIT_CYCLE.md
echo   tests/TEST_SCRIPTS.md
goto end

:no_args
echo [ERROR] Не указана метка времени
echo Использование: audit\check_context_changes.bat ^<timestamp^>
echo Метка времени хранится в КОНТЕКСТЕ БЕСЕДЫ.
echo Получить текущий: powershell -Command "[int][double]::Parse((Get-Date -UFormat %%s))"

:end
endlocal
