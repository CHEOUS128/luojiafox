@echo off
REM Pre-push checklist. Pure ASCII on purpose: no codepage issues.
REM 1) shows what is modified / untracked  2) blocks files over 95 MB
REM    3) reminds you that push is yours to run.
chcp 437 >nul
setlocal
cd /d "%~dp0"
echo === 1. repo root ===
git rev-parse --show-toplevel
echo.
echo === 2. modified (tracked) ===
git status --short --untracked-files=no
echo.
echo === 3. untracked, collapsed by dir ===
git ls-files --others --exclude-standard --directory
echo.
echo === 4. staged, files over 95 MB (GitHub hard limit is 100 MB) ===
for /f "usebackq delims=" %%f in (`git diff --cached --name-only --diff-filter=ACM`) do (
  if exist "%%f" (
    for %%s in ("%%f") do (
      if %%~zs GTR 104857600 echo   TOO BIG: %%f  %%~zs bytes
    )
  )
)
echo   (nothing above = ok)
echo.
echo === 5. ahead/behind origin/main ===
git rev-list --left-right --count origin/main...HEAD 2>nul
echo   (left = behind, right = ahead; push when right > 0)
echo.
echo === 6. ignored large files that must stay out ===
git check-ignore -v luojiamaster.zip compression-war/ffmpeg.exe compression-war/cw_sim.exe 2>nul
echo.
echo Reminder: run  git push origin main  yourself. Never push from an AI sandbox.
pause
