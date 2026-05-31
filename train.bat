@echo off
REM =============================================
REM  sofcon 学習スクリプト
REM  ダブルクリックで実行 / 再実行で自動再開
REM =============================================
cd /d "%~dp0"

echo [%date% %time%] Starting / Resuming training... >> train\log.txt

python train\self_play.py ^
  --iters        300 ^
  --sims          50 ^
  --games          4 ^
  --players_min    2 ^
  --players_max    6 ^
  --board_h        9 ^
  --board_w        9 ^
  --device       cpu ^
  >> train\log.txt 2>&1

echo [%date% %time%] Process exited with code %errorlevel% >> train\log.txt
pause
