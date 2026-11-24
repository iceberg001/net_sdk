cmake_minimum_required(VERSION 3.15)
project(net_sdk VERSION 1.3.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 预编译的第三方库 hv_static.lib 使用静态 CRT 编译（Release=/MT, Debug=/MTd）。
# 所有目标必须使用相同的运行时库，否则 MSVC 链接时报 LNK2038 RuntimeLibrary 不匹配。
#
# NET_SDK_CRT_DYNAMIC=ON 时改用动态 CRT（/MD /MDd），供以 /MD 编译的宿主工程
# （如 up_live）静态链接使用；此时必须同时把 libhv 换成 /MD 版本，否则链接报
# LNK2038。默认 OFF 保持既有静态 CRT 配置。
option(NET_SDK_CRT_DYNAMIC "Build with dynamic CRT (/MD /MDd) to match /MD hosts" OFF)
if(MSVC)
    if(NET_SDK_CRT_DYNAMIC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# 默认静态库：预编译 hv_static.lib 使用静态 CRT (/MT /MTd)，DLL 与 EXE 各有独立 CRT 堆。
# C++ STL 对象（std::string/std::function/回调返回值）跨模块分配/释放会破坏堆，
# Debug CRT 报 _act_first_block == header，Release 下则是静默隐患。
# 静态链接让 SDK + libhv + 消费方共享同一 CRT 堆，是唯一安全的生产配置。
option(NET_SDK_BUILD_SHARED "Build shared library (not recommended: static CRT heap hazard)" OFF)
if(NET_SDK_BUILD_SHARED AND MSVC AND NOT NET_SDK_CRT_DYNAMIC)
    message(WARNING
        "NET_SDK_BUILD_SHARED=ON with the bundled libhv (built with static CRT /MT) is "
        "unsafe on Windows: C++ STL objects crossing the DLL boundary corrupt the CRT heap. "
        "Use the default static build for production.")
endif()
option(NET_SDK_BUILD_EXAMPLES "Build example programs" ON)
option(NET_SDK_BUILD_TESTS "Build automated tests" ON)
# 随 SDK 发布的 libhv 已用 OpenSSL 1.1.1 重新编译（WITH_OPENSSL），默认支持 HTTPS。
# 关闭此项仅用于回退到 nossl 版 libhv 的特殊场景。
option(NET_SDK_WITH_SSL "libhv was built with SSL support" ON)

set(NET_SDK_VERSION_MAJOR 1)
set(NET_SDK_VERSION_MINOR 3)
set(NET_SDK_VERSION_PATCH 0)

set(THIRDPARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
set(LIBHV_INCLUDE_DIR ${THIRDPARTY_DIR}/libhv/include)
# /MD 宿主集成时，libhv 也必须用 /MD 重编；允许通过 cache 变量覆盖库目录。
set(NET_SDK_LIBHV_LIB_DIR "${THIRDPARTY_DIR}/libhv/lib" CACHE PATH "libhv library directory")
set(LIBHV_LIB_DIR ${NET_SDK_LIBHV_LIB_DIR})
set(OPENSSL_DIR ${THIRDPARTY_DIR}/openssl)
set(SQLITE_DIR ${THIRDPARTY_DIR}/sqlite)
set(OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/output)

set(NET_SDK_PUBLIC_HEADERS
    include/net_sdk/Common.h
    include/net_sdk/Logger.h
    include/net_sdk/ThreadPool.h
    include/net_sdk/EventLoopPool.h
    include/net_sdk/SignalSlot.h
    include/net_sdk/json.hpp
    include/net_sdk/JsonHelper.h
    include/net_sdk/SqliteHelper.h
    include/net_sdk/CommandRequest.h
    include/net_sdk/MainThreadDispatcher.h
    include/net_sdk/HttpClient.h
    include/net_sdk/TcpClient.h
    include/net_sdk/TcpServer.h
    include/net_sdk/NetSdk.h
)

set(NET_SDK_SOURCES
    src/Logger.cpp
    src/ThreadPool.cpp
    src/EventLoopPool.cpp
    src/SignalSlot.cpp
    src/JsonHelper.cpp
    src/SqliteHelper.cpp
    src/CommandRequest.cpp
    src/MainThreadDispatcher.cpp
    src/HttpClient.cpp
    src/TcpClient.cpp
    src/TcpServer.cpp
    src/NetSdk.cpp
    ${SQLITE_DIR}/sqlite3.c
)

if(NET_SDK_BUILD_SHARED)
    add_library(net_sdk SHARED ${NET_SDK_SOURCES} ${NET_SDK_PUBLIC_HEADERS})
else()
    add_library(net_sdk STATIC ${NET_SDK_SOURCES} ${NET_SDK_PUBLIC_HEADERS})
endif()

# SDK 用静态 CRT 编译，消费者必须使用同一 CRT（/MT /MTd），否则 LNK2038。
# 通过 INTERFACE 属性把该约束传递给 find_package 的消费目标。
if(MSVC)
    if(NET_SDK_CRT_DYNAMIC)
        set(_sdk_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(_sdk_crt "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    set_target_properties(net_sdk PROPERTIES
        INTERFACE_MSVC_RUNTIME_LIBRARY "${_sdk_crt}"
    )
endif()

target_include_directories(net_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # libhv/OpenSSL 头文件仅在 SDK 内部实现中使用（hloop/http_client 等），
        # 不暴露给消费者；公共 API 依赖的 json.hpp 已随 SDK 头文件一起发布。
        ${LIBHV_INCLUDE_DIR}
        ${OPENSSL_DIR}/include
        ${SQLITE_DIR}
)

if(MSVC)
    # 第三方库以 BUILD/INSTALL interface 区分路径：构建树内用源码树绝对路径，
    # 安装后用相对安装前缀的路径，避免把开发机路径写死进导出的 Targets 文件。
    # $<IF:$<CONFIG:Debug>,...> 兼容 VS 多配置生成器。
    set(_HV_LIB "$<IF:$<CONFIG:Debug>,${LIBHV_LIB_DIR}/Debug/hv_static.lib,${LIBHV_LIB_DIR}/Release/hv_static.lib>")
    # 安装后路径必须以 ${_IMPORT_PREFIX} 开头，CMake 才会当作带路径的文件而非库名
    # （否则会错误追加 .lib 变成 hv_static.lib.lib）。\${...} 在 configure 期不展开。
    set(_P "\${_IMPORT_PREFIX}/")
    set(_HV_LIB_INST "$<IF:$<CONFIG:Debug>,${_P}${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/hv/Debug/hv_static.lib,${_P}${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/hv/Release/hv_static.lib>")

    target_link_libraries(net_sdk PRIVATE
        "$<BUILD_INTERFACE:${_HV_LIB}>"
        "$<INSTALL_INTERFACE:${_HV_LIB_INST}>"
    )
    if(NET_SDK_WITH_SSL)
        target_link_libraries(net_sdk PRIVATE
            "$<BUILD_INTERFACE:${OPENSSL_DIR}/lib/libssl.lib>"
            "$<BUILD_INTERFACE:${OPENSSL_DIR}/lib/libcrypto.lib>"
            "$<INSTALL_INTERFACE:${_P}${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/openssl/libssl.lib>"
            "$<INSTALL_INTERFACE:${_P}${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/openssl/libcrypto.lib>"
            crypt32
        )
    endif()
    target_link_libraries(net_sdk PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    find_library(LIBHV_LIB NAMES hv_static hv PATHS ${LIBHV_LIB_DIR})
    if(NOT LIBHV_LIB)
        message(FATAL_ERROR
            "libhv 静态库未找到（${LIBHV_LIB_DIR}）。请先在 Linux/macOS 上编译 libhv 并放置 libhv_static.a 到该目录。")
    endif()
    target_link_libraries(net_sdk PRIVATE ${LIBHV_LIB} Threads::Threads)
endif()

set_target_properties(net_sdk PROPERTIES
    VERSION ${NET_SDK_VERSION_MAJOR}.${NET_SDK_VERSION_MINOR}.${NET_SDK_VERSION_PATCH}
    SOVERSION ${NET_SDK_VERSION_MAJOR}
)

# 符号导出：Windows 用 WINDOWS_EXPORT_ALL_SYMBOLS 自动导出；
# 非 Windows 平台若设置 hidden 且没有 visibility("default") 标注，
# .so 将不导出任何 SDK 符号导致消费者链接失败，因此这里保持默认可见性。
if(MSVC)
    set_target_properties(net_sdk PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# 使用 per-config 输出目录：VS 多配置生成器会自动在基础目录后追加配置名，
# 若再手动拼接 ${CMAKE_BUILD_TYPE} 会得到 .../Release/Release 这样的重复路径。
# 同时按 shared/static 与 CRT 风味区分目录：
# /MD 构建输出到 static_md/，避免覆盖默认 /MT 产物（static/），
# 两种 CRT 的库可以并存并各自归档给不同宿主工程使用。
if(NET_SDK_BUILD_SHARED)
    set(_lib_flavor shared)
elseif(NET_SDK_CRT_DYNAMIC)
    set(_lib_flavor static_md)
else()
    set(_lib_flavor static)
endif()
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set(_out "${OUTPUT_DIR}/lib/${_lib_flavor}/${_cfg}")
    set_target_properties(net_sdk PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_out}"
        ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${_out}"
        LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_out}"
        PDB_OUTPUT_DIRECTORY_${_CFG}    "${_out}"
    )
endforeach()

# libhv 以静态库(hv_static.lib)链接，必须定义 HV_STATICLIB，
# 否则其头文件会把符号声明为 dllimport，导致 LNK2019 无法解析。
target_compile_definitions(net_sdk PRIVATE HV_STATICLIB)
if(NET_SDK_WITH_SSL)
    target_compile_definitions(net_sdk PRIVATE NET_SDK_HAS_SSL)
    # 新 libhv 用 WITH_OPENSSL 编译；SDK 侧同名定义可防止 hssl.h 在 Windows 上
    # 误回退到 WITH_WINTLS，保证头文件与库的 SSL 后端 ABI 一致。
    target_compile_definitions(net_sdk PRIVATE WITH_OPENSSL)
endif()

if(MSVC)
    target_compile_options(net_sdk PRIVATE /W3 /utf-8 /Zi)
    target_compile_definitions(net_sdk PRIVATE _CRT_SECURE_NO_WARNINGS SQLITE_THREADSAFE=1)
    # PDB 名称按配置区分，避免 Debug/Release 互相覆盖
    foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
        set_target_properties(net_sdk PROPERTIES
            COMPILE_PDB_NAME_${_cfg} net_sdk_${_cfg}
        )
    endforeach()
else()
    target_compile_options(net_sdk PRIVATE -Wall -Wextra -Wno-unused-parameter -g)
    target_compile_definitions(net_sdk PRIVATE SQLITE_THREADSAFE=1)
endif()

# 发布自包含的头文件树：所有公共头（含 json.hpp）都在 net_sdk/ 目录下，
# 消费者只需一个 include 根即可，无需 libhv/ 等额外子目录。
add_custom_command(TARGET net_sdk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/include/net_sdk
        ${OUTPUT_DIR}/include/net_sdk
    COMMENT "Copying public headers to output directory"
)

# OpenSSL 通过 DLL 方式链接（内部使用自己的 CRT，避免与 SDK 的 /MT 堆混用）。
# 把 DLL 复制到与 SDK 库同目录，随产物一起发布。
if(MSVC AND NET_SDK_WITH_SSL)
    add_custom_command(TARGET net_sdk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libssl-1_1-x64.dll $<TARGET_FILE_DIR:net_sdk>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libcrypto-1_1-x64.dll $<TARGET_FILE_DIR:net_sdk>
        COMMENT "Copying OpenSSL runtime DLLs next to net_sdk"
    )
endif()

if(NET_SDK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(NET_SDK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# 安装与打包：生成可被 find_package(NetSdk) 直接消费的 CMake Package
# ============================================================================
include(CMakePackageConfigHelpers)

install(TARGETS net_sdk
    EXPORT NetSdkTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Windows DLL / exe
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}   # Linux .so / macOS .dylib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}   # 静态库 / 导入库
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 头文件树：SDK 公共头 + json.hpp（都在 net_sdk/ 目录内，统一发布）
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/net_sdk
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# 第三方运行时与库随 SDK 一起安装，保证消费者部署完整。
if(MSVC AND NET_SDK_WITH_SSL)
    install(FILES
        ${OPENSSL_DIR}/bin/libssl-1_1-x64.dll
        ${OPENSSL_DIR}/bin/libcrypto-1_1-x64.dll
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
install(FILES
    ${LIBHV_LIB_DIR}/Release/hv_static.lib
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/hv/Release
)
install(FILES
    ${LIBHV_LIB_DIR}/Debug/hv_static.lib
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/hv/Debug
)
if(MSVC AND NET_SDK_WITH_SSL)
    install(FILES
        ${OPENSSL_DIR}/lib/libssl.lib
        ${OPENSSL_DIR}/lib/libcrypto.lib
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/net_sdk/thirdparty/openssl
    )
endif()

set(NET_SDK_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/NetSdk)

install(EXPORT NetSdkTargets
    FILE NetSdkTargets.cmake
    NAMESPACE NetSdk::
    DESTINATION ${NET_SDK_INSTALL_CMAKEDIR}
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetSdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NetSdkConfig.cmake
    INSTALL_DESTINATION ${NET_SDK_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NetSdkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/NetSdkConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/NetSdkConfigVersion.cmake
    DESTINATION ${NET_SDK_INSTALL_CMAKEDIR}
)

message(STATUS "NetSdk ${PROJECT_VERSION} configured (shared=${NET_SDK_BUILD_SHARED}, examples=${NET_SDK_BUILD_EXAMPLES})")