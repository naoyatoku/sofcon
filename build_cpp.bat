@echo off
REM =============================================
REM  C++ MCTSプレイヤーのビルド・動作確認
REM  ダブルクリックで実行（WSL/Ubuntuを使用）
REM =============================================

echo === Building self-test (via WSL) ===
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 -pthread -DMCTS_SELFTEST mcts_player.cpp -o mcts_test 2>&1"
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )

echo === Running self-test (Time-based MCTS vs random) ===
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && ./mcts_test"

echo.
echo === Building speed benchmark ===
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 bench.cpp -o bench 2>&1"
if errorlevel 1 ( echo BUILD FAILED & pause & exit /b 1 )
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && ./bench"

echo.
echo Done.
pause
