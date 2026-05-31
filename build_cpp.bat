@echo off
REM =============================================
REM  C++ MCTSプレイヤーのビルド＆動作確認
REM  ダブルクリックで実行
REM =============================================
cd /d "%~dp0\cpp"

echo === Building self-test ===
g++ -O2 -std=c++17 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test.exe
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )

echo === Running self-test (MCTS vs random) ===
mcts_test.exe

echo.
echo === Building 15x15 speed benchmark ===
g++ -O2 -std=c++17 bench.cpp -o bench.exe
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )
bench.exe

echo.
echo Done.
pause
