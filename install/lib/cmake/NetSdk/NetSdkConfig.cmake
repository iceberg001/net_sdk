
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was NetSdkConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)

# libhv / SQLite 均以静态方式编译进 SDK 内部，消费者无需也无法单独查找它们。
# 这里只需引入本 SDK 导出的目标。

include("${CMAKE_CURRENT_LIST_DIR}/NetSdkTargets.cmake")

# CRT 必须与 SDK 构建时一致，否则链接时报 LNK2038。
# 静态库无法强制改变消费目标的 CRT，因此在配置期检测并给出清晰指引。
# 提示内容由构建选项 NET_SDK_CRT_DYNAMIC 决定（/MT 静态 或 /MD 动态）。
if(MSVC)
    if("OFF" STREQUAL "ON")
        set(_NETSDK_CRT_EXPECTED "MultiThreaded\\$<\\$<CONFIG:Debug>:Debug>DLL")
        set(_NETSDK_CRT_DESC     "动态 CRT (/MD, /MDd)")
        set(_NETSDK_CRT_PATTERN  "MultiThreaded.*DLL")
    else()
        set(_NETSDK_CRT_EXPECTED "MultiThreaded\\$<\\$<CONFIG:Debug>:Debug>")
        set(_NETSDK_CRT_DESC     "静态 CRT (/MT, /MTd)")
        set(_NETSDK_CRT_PATTERN  "MultiThreaded")
    endif()
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY)
        message(WARNING
            "NetSdk 使用${_NETSDK_CRT_DESC}编译。请在 find_package(NetSdk) 之前设置：\n"
            "  set(CMAKE_MSVC_RUNTIME_LIBRARY \"${_NETSDK_CRT_EXPECTED}\")\n"
            "否则链接会出现 LNK2038 RuntimeLibrary 不匹配。")
    elseif(NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "${_NETSDK_CRT_PATTERN}")
        message(WARNING
            "NetSdk 需要${_NETSDK_CRT_DESC}，但当前 CMAKE_MSVC_RUNTIME_LIBRARY="
            "'${CMAKE_MSVC_RUNTIME_LIBRARY}'，链接可能出现 LNK2038 不匹配。")
    endif()
    unset(_NETSDK_CRT_EXPECTED)
    unset(_NETSDK_CRT_DESC)
    unset(_NETSDK_CRT_PATTERN)
endif()

# OpenSSL 运行时 DLL 随 SDK 安装在 <prefix>/bin。SDK 以导入库方式链接，
# 消费方可执行文件必须能找到这两个 DLL，否则启动报 0xC0000135 / 加载失败。
# 提供便捷函数，避免每个项目手写复制规则：
#   find_package(NetSdk REQUIRED)
#   add_executable(app main.cpp)
#   target_link_libraries(app PRIVATE NetSdk::net_sdk)
#   net_sdk_copy_runtime_dlls(app)
set(NetSdk_RUNTIME_DLLS
    "${CMAKE_CURRENT_LIST_DIR}/../../../bin/libssl-1_1-x64.dll"
    "${CMAKE_CURRENT_LIST_DIR}/../../../bin/libcrypto-1_1-x64.dll")
function(net_sdk_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    foreach(_netsdk_dll IN LISTS NetSdk_RUNTIME_DLLS)
        if(EXISTS "${_netsdk_dll}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_netsdk_dll}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying NetSdk OpenSSL runtime DLL next to ${target}"
                VERBATIM)
        endif()
    endforeach()
endfunction()

check_required_components(NetSdk)
