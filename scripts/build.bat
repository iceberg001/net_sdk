@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  NetSDK build script  (Windows / MSVC)
rem  Full documentation: see README.md
rem
rem  Usage:  scripts\build.bat [options]
rem
rem    flavors  : mt      static lib + static CRT  /MT   (default, recommended)
rem               md      static lib + dynamic CRT /MD   (for /MD host projects)
rem               shared  DLL + static CRT /MT           (not recommended)
rem               all     build mt + md
rem    configs  : release (default) | debug | both
rem    switches : test    run ctest after build (implies tests target)
rem               install run cmake --install (mt flavor only)
rem               clean   delete the build dir before configuring
rem               full    build examples+tests for md/shared too
rem
rem  Examples:
rem    scripts\build.bat                     (mt / Release, examples + tests)
rem    scripts\build.bat mt both test        (mt, Release+Debug, run ctest)
rem    scripts\build.bat md release          (md lib for /MD host projects)
rem    scripts\build.bat all both test install clean
rem ============================================================

rem Proxy env vars can crash MSBuild in some environments; clear them.
set "http_proxy="
set "https_proxy="
set "HTTP_PROXY="
set "HTTPS_PROXY="

set "FLAVOR=mt"
set "CONFIG=release"
set "RUN_TEST=0"
set "DO_INSTALL=0"
set "DO_CLEAN=0"
set "DO_FULL=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="mt"      set "FLAVOR=mt"
if /I "%~1"=="md"      set "FLAVOR=md"
if /I "%~1"=="shared"  set "FLAVOR=shared"
if /I "%~1"=="all"     set "FLAVOR=all"
if /I "%~1"=="release" set "CONFIG=release"
if /I "%~1"=="debug"   set "CONFIG=debug"
if /I "%~1"=="both"    set "CONFIG=both"
if /I "%~1"=="test"    set "RUN_TEST=1"
if /I "%~1"=="install" set "DO_INSTALL=1"
if /I "%~1"=="clean"   set "DO_CLEAN=1"
if /I "%~1"=="full"    set "DO_FULL=1"
shift
goto parse_args
:args_done

set "FLAVORS=%FLAVOR%"
if /I "%FLAVOR%"=="all" set "FLAVORS=mt md"

set "CONFIGS=Release"
if /I "%CONFIG%"=="debug" set "CONFIGS=Debug"
if /I "%CONFIG%"=="both"  set "CONFIGS=Release Debug"

rem Project root = scripts dir parent. Normalize with ~f to avoid ".." in paths.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

for %%F in (%FLAVORS%) do (
    call :build_flavor %%F
    if errorlevel 1 (
        echo.
        echo [ERROR] flavor '%%F' failed. Build aborted.
        exit /b 1
    )
)
echo.
echo ========================================
echo   Build completed successfully!
echo   Libraries: %ROOT%\output\lib\{static^|static_md^|shared}\{Release^|Debug}
echo   Headers:   %ROOT%\output\include\net_sdk
echo ========================================
exit /b 0

rem ------------------------------------------------------------
rem %1 = flavor: mt | md | shared
:build_flavor
set "F=%~1"
set "BUILD_DIR=build"
set "CRT=OFF"
set "SHARED=OFF"
set "HVLIB=thirdparty\libhv\lib"
set "EXAMPLES=OFF"
set "TESTS=OFF"
if /I "%F%"=="mt" (
    set "BUILD_DIR=build"
    set "EXAMPLES=ON"
    set "TESTS=ON"
) else if /I "%F%"=="md" (
    set "BUILD_DIR=build-md"
    set "CRT=ON"
    set "HVLIB=thirdparty\libhv\lib_md"
) else if /I "%F%"=="shared" (
    set "BUILD_DIR=build-shared"
    set "SHARED=ON"
)
if "%DO_FULL%"=="1" (
    set "EXAMPLES=ON"
    set "TESTS=ON"
)
if "%RUN_TEST%"=="1" set "TESTS=ON"

if "%DO_CLEAN%"=="1" (
    if exist "!ROOT!\!BUILD_DIR!" rmdir /s /q "!ROOT!\!BUILD_DIR!"
)

echo.
echo ========================================
echo   Building flavor: %F%   (configs: %CONFIGS%)
echo   build dir: !BUILD_DIR!   CRT dynamic: !CRT!   shared: !SHARED!
echo ========================================

echo [1/3] Configuring CMake...
cmake -S "!ROOT!" -B "!ROOT!\!BUILD_DIR!" ^
    -DCMAKE_INSTALL_PREFIX="!ROOT!\install" ^
    -DNET_SDK_BUILD_SHARED=!SHARED! ^
    -DNET_SDK_CRT_DYNAMIC=!CRT! ^
    -DNET_SDK_BUILD_EXAMPLES=!EXAMPLES! ^
    -DNET_SDK_BUILD_TESTS=!TESTS! ^
    -DNET_SDK_LIBHV_LIB_DIR="!ROOT!\!HVLIB!"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

for %%C in (%CONFIGS%) do (
    echo.
    echo [2/3] Building %F% %%C ...
    cmake --build "!ROOT!\!BUILD_DIR!" --config %%C --parallel
    if errorlevel 1 (
        echo [ERROR] Build failed: %F% %%C
        exit /b 1
    )
    if "%RUN_TEST%"=="1" (
        echo.
        echo [3/3] Running ctest: %F% %%C ...
        ctest --test-dir "!ROOT!\!BUILD_DIR!" -C %%C --output-on-failure
        if errorlevel 1 (
            echo [ERROR] Tests failed: %F% %%C
            exit /b 1
        )
    )
)

if "%DO_INSTALL%"=="1" (
    if /I "%F%"=="mt" (
        echo.
        echo Installing Release flavor into !ROOT!\install ...
        cmake --install "!ROOT!\!BUILD_DIR!" --config Release
        if errorlevel 1 (
            echo [WARNING] Install step had issues, but build succeeded.
        )
    ) else (
        echo.
        echo [NOTE] install skipped for flavor '%F%': the install tree is
        echo        generated from the default mt flavor to keep the CMake
        echo        package's CRT hint consistent. The %F% library is
        echo        available under output\lib\.
    )
)
exit /b 0
