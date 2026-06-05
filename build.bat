@echo off
REM ============================================================
REM  prog.cpp -> prog.dll ビルドスクリプト  (MSYS2 MinGW-w64)
REM
REM  使い方: このファイルをダブルクリック
REM  出力:   prog.dll  (提出ファイル)
REM          libwinpthread-1.dll  (prog.dllと同じフォルダに置く)
REM ============================================================
setlocal

set GCC=C:\msys64\mingw64\bin\g++.exe
set OBJ=C:\msys64\mingw64\bin\objdump.exe
set PTHREAD_DLL=C:\msys64\mingw64\bin\libwinpthread-1.dll
set DIR=%~dp0

REM PATH に MSYS2 を追加（g++の実行に必要）
set PATH=%PATH%;C:\msys64\mingw64\bin;C:\msys64\usr\bin

cd /d "%DIR%"

REM entry.h がなければダミーを作成
if not exist entry.h (
    echo // dummy entry.h > entry.h
    set DUMMY_ENTRY=1
)

echo.
echo Building prog.dll ...
%GCC% -O2 -std=c++14 -shared ^
    -static-libgcc -static-libstdc++ ^
    -Wl,--whole-archive -lwinpthread -Wl,--no-whole-archive ^
    -o prog.dll prog.cpp

if %errorlevel% neq 0 (
    echo.
    echo BUILD FAILED
    if defined DUMMY_ENTRY del entry.h
    pause
    exit /b 1
)

echo BUILD OK
echo.

REM 必要なDLLをコピー
copy /y "%PTHREAD_DLL%" "%DIR%libwinpthread-1.dll" > nul
echo Copied: libwinpthread-1.dll

echo.
echo === Dependencies ===
%OBJ% -p prog.dll | findstr "DLL Name"

echo.
echo === File sizes ===
dir /b prog.dll libwinpthread-1.dll

if defined DUMMY_ENTRY del entry.h

echo.
echo Done.  Submit: prog.dll + libwinpthread-1.dll
pause
endlocal
