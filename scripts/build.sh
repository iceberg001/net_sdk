#!/usr/bin/env bash
# ============================================================
#  NetSDK build script  (Linux / macOS)
#  Full documentation: see README.md
#
#  Usage:  scripts/build.sh [options]
#
#    configs  : release (default) | debug
#    flavors  : static (default) | shared
#    switches : ssl      link SSL-enabled libhv (must be built by yourself)
#               test     run ctest after build
#               install  run cmake --install
#               clean    delete the build dir before configuring
#
#  Prerequisites (IMPORTANT, Linux/macOS only):
#    The repository ships prebuilt libhv for Windows only. On Linux/macOS
#    you must build libhv yourself and place the static library at
#        thirdparty/libhv/lib/libhv_static.a        (release)
#        thirdparty/libhv/lib/libhv_staticd.a       (debug, optional)
#    HTTPS is disabled by default (NET_SDK_WITH_SSL=OFF) because no
#    prebuilt SSL-enabled libhv is shipped for these platforms; pass
#    'ssl' after building libhv with WITH_OPENSSL=ON.
#
#  Examples:
#    scripts/build.sh
#    scripts/build.sh debug test
#    scripts/build.sh release ssl install
# ============================================================
set -euo pipefail

BUILD_TYPE=Release
BUILD_SHARED=OFF
WITH_SSL=OFF
RUN_TEST=0
DO_INSTALL=0
DO_CLEAN=0
BUILD_DIR=build

for arg in "$@"; do
    case "$arg" in
        release) BUILD_TYPE=Release ;;
        debug)   BUILD_TYPE=Debug ;;
        shared)  BUILD_SHARED=ON ;;
        static)  BUILD_SHARED=OFF ;;
        ssl)     WITH_SSL=ON ;;
        test)    RUN_TEST=1 ;;
        install) DO_INSTALL=1 ;;
        clean)   DO_CLEAN=1 ;;
        *) echo "Unknown option: $arg (see header of this script)"; exit 1 ;;
    esac
done

cd "$(dirname "$0")/.."

if [ "$DO_CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

echo "========================================"
echo "  NetSDK build ($BUILD_TYPE, shared=$BUILD_SHARED, ssl=$WITH_SSL)"
echo "========================================"

echo ""
echo "[1/3] Configuring CMake..."
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX=install \
    -DNET_SDK_BUILD_SHARED="$BUILD_SHARED" \
    -DNET_SDK_WITH_SSL="$WITH_SSL" \
    -DNET_SDK_BUILD_EXAMPLES=ON \
    -DNET_SDK_BUILD_TESTS=ON

echo ""
echo "[2/3] Building..."
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [ "$RUN_TEST" = "1" ]; then
    echo ""
    echo "[3/3] Running ctest..."
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

if [ "$DO_INSTALL" = "1" ]; then
    echo ""
    echo "Installing into ./install ..."
    cmake --install "$BUILD_DIR"
fi

echo ""
echo "========================================"
echo "  Build completed successfully!"
echo "  Build dir: $BUILD_DIR/"
echo "  Install:   ./install/"
echo "========================================"
