set(NET_SDK_EXAMPLES
    http_sync_example
    http_async_example
    tcp_client_example
    tcp_server_example
    signal_slot_example
    command_request_example
    sqlite_example
    logger_example
)

foreach(example ${NET_SDK_EXAMPLES})
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE net_sdk)
    if(MSVC)
        target_compile_options(${example} PRIVATE /utf-8)
    endif()
    # 共享库时把 DLL 复制到示例同目录，保证示例开箱即可运行；
    # 静态库时 net_sdk 已直接链接进 exe，无需复制（复制 .lib 也没有意义）。
    if(NET_SDK_BUILD_SHARED)
        add_custom_command(TARGET ${example} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:net_sdk> $<TARGET_FILE_DIR:${example}>
            COMMENT "Copying net_sdk runtime library next to ${example}"
        )
    endif()
    # HTTPS 依赖 OpenSSL 的运行时 DLL，需随可执行文件一起部署。
    if(MSVC AND NET_SDK_WITH_SSL)
        add_custom_command(TARGET ${example} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libssl-1_1-x64.dll $<TARGET_FILE_DIR:${example}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libcrypto-1_1-x64.dll $<TARGET_FILE_DIR:${example}>
            COMMENT "Copying OpenSSL runtime DLLs next to ${example}"
        )
    endif()
endforeach()