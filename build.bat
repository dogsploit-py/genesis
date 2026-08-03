@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem GENESIS build script (MinGW-w64 / TDM-GCC).
rem
rem Produces a single self-contained Genesis.exe. Everything is either compiled
rem from vendored source or is an OS DLL, so there is nothing to install and
rem nothing to ship alongside the executable.
rem
rem   build.bat            release build
rem   build.bat debug      debug build with symbols and assertions
rem   build.bat clean      remove build artefacts
rem ===========================================================================

set MODE=%1
if "%MODE%"=="" set MODE=release

if /I "%MODE%"=="clean" (
    if exist build rmdir /s /q build
    if exist Genesis.exe del Genesis.exe
    echo Cleaned.
    goto :eof
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo ERROR: g++ was not found on PATH.
    echo Install MinGW-w64 or TDM-GCC and add its bin directory to PATH.
    exit /b 1
)

if not exist vendor\imgui\imgui.cpp (
    echo ERROR: vendor\imgui is missing.
    echo Run:  git clone --depth 1 -b docking https://github.com/ocornut/imgui.git vendor\imgui
    exit /b 1
)

if not exist build mkdir build

set INCLUDES=-Isrc -Ivendor\imgui -Ivendor\imgui\backends -Ivendor\lua
set WARN=-Wall -Wextra
set COMMON=-std=c++17 -m64 %WARN% %INCLUDES% -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX

if /I "%MODE%"=="debug" (
    set CXXFLAGS=%COMMON% -g -O0 -DDEBUG
    set LDFLAGS=-m64
) else (
    rem -O2 with default (IEEE-correct) float semantics. -ffast-math is
    rem deliberately NOT used: it would break bit-for-bit reproducibility.
    set CXXFLAGS=%COMMON% -O2 -DNDEBUG -fno-fast-math
    set LDFLAGS=-m64 -mwindows -s
)

rem ImGui itself is third-party; its warnings are not ours to fix, so it is
rem compiled without -Wall -Wextra while our own sources keep them enabled.
set IMGUI_FLAGS=-std=c++17 -m64 -O2 %INCLUDES% -DWIN32_LEAN_AND_MEAN -DNOMINMAX

set SOURCES=^
 src\main.cpp ^
 src\core\config.cpp ^
 src\core\noise.cpp ^
 src\core\json.cpp ^
 src\core\profiler.cpp ^
 src\core\serialize.cpp ^
 src\sim\time.cpp ^
 src\sim\world.cpp ^
 src\sim\worldgen.cpp ^
 src\sim\genetics.cpp ^
 src\sim\brain.cpp ^
 src\sim\attraction.cpp ^
 src\sim\agent.cpp ^
 src\sim\simulation.cpp ^
 src\chem\elements.cpp ^
 src\chem\reactions.cpp ^
 src\chem\materials.cpp ^
 src\sim\knowledge.cpp ^
 src\sim\species.cpp ^
 src\sim\chemistry_agent.cpp ^
 src\econ\economy.cpp ^
 src\god\god.cpp ^
 src\god\lua_api.cpp ^
 src\ui\app.cpp ^
 src\ui\panels.cpp ^
 src\ui\viewport.cpp ^
 src\ui\cards.cpp ^
 src\ui\chem_ui.cpp ^
 src\ui\profiler_ui.cpp ^
 src\ui\phylogeny_ui.cpp ^
 src\ui\econ_ui.cpp ^
 src\ui\god_ui.cpp

set IMGUI_SOURCES=^
 vendor\imgui\imgui.cpp ^
 vendor\imgui\imgui_draw.cpp ^
 vendor\imgui\imgui_tables.cpp ^
 vendor\imgui\imgui_widgets.cpp ^
 vendor\imgui\imgui_demo.cpp ^
 vendor\imgui\backends\imgui_impl_win32.cpp ^
 vendor\imgui\backends\imgui_impl_opengl2.cpp

set OBJS=

echo Compiling Lua 5.4...
rem lua.c and luac.c hold their own main(); onelua.c is the amalgamation and
rem ltests.c is the internal test harness. All four are excluded.
for %%F in (vendor\lua\*.c) do (
    set NAME=%%~nF
    if /I not "!NAME!"=="lua" if /I not "!NAME!"=="luac" if /I not "!NAME!"=="onelua" if /I not "!NAME!"=="ltests" (
        rem Prefixed lualib_ rather than lua_, so a Lua source can never
        rem collide with our own src/god/lua_api.cpp object name.
        if not exist build\lualib_!NAME!.o (
            gcc -O2 -m64 -w -Ivendor\lua -c "%%F" -o "build\lualib_!NAME!.o" || exit /b 1
        )
        set OBJS=!OBJS! build\lualib_!NAME!.o
    )
)

echo Compiling Dear ImGui...
for %%F in (%IMGUI_SOURCES%) do (
    set NAME=%%~nF
    if not exist build\imgui_!NAME!.o (
        g++ %IMGUI_FLAGS% -c "%%F" -o "build\imgui_!NAME!.o" || exit /b 1
    )
    set OBJS=!OBJS! build\imgui_!NAME!.o
)

echo Compiling GENESIS...
for %%F in (%SOURCES%) do (
    set NAME=%%~nF
    g++ %CXXFLAGS% -c "%%F" -o "build\!NAME!.o"
    if errorlevel 1 (
        echo.
        echo *** BUILD FAILED while compiling %%F ***
        exit /b 1
    )
    set OBJS=!OBJS! build\!NAME!.o
)

echo Linking...
rem Static libgcc/libstdc++/winpthread so the exe has no runtime dependency
rem beyond the OS DLLs it links directly.
g++ %LDFLAGS% !OBJS! -o Genesis.exe ^
    -static -static-libgcc -static-libstdc++ ^
    -lopengl32 -lgdi32 -luser32 -lshell32 -ldwmapi -limm32
if errorlevel 1 (
    echo.
    echo *** BUILD FAILED at link ***
    exit /b 1
)

if not exist data mkdir data
if not exist data\config.ini Genesis.exe --write-config >nul 2>nul

echo.
echo Built Genesis.exe (%MODE%)
for %%A in (Genesis.exe) do echo Size: %%~zA bytes
endlocal
