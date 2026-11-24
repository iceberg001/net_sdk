# 自动化测试套件：不依赖外网，网络场景使用本机 loopback。
# 每个测试是一个独立可执行文件，便于定位失败并接入 CTest。

set(NET_SDK_TESTS
    test_logger
    test_json
    test_signalslot
    test_command
    test_dispatcher
    test_tcp
    test_lifecycle
    test_https
    test_http_concurrency
    test_tcp_concurrency
    test_callback_poster
)

foreach(t ${NET_SDK_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE net_sdk)
    if(MSVC)
        target_compile_options(${t} PRIVATE /utf-8)
    endif()
    # 共享库时把 DLL 复制到测试程序同目录，保证 ctest 直接可运行。
    if(NET_SDK_BUILD_SHARED)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:net_sdk> $<TARGET_FILE_DIR:${t}>
            COMMENT "Copying net_sdk runtime library next to ${t}"
        )
    endif()
    # HTTPS 依赖 OpenSSL 的运行时 DLL，需随测试程序一起部署。
    if(MSVC AND NET_SDK_WITH_SSL)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libssl-1_1-x64.dll $<TARGET_FILE_DIR:${t}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DIR}/bin/libcrypto-1_1-x64.dll $<TARGET_FILE_DIR:${t}>
            COMMENT "Copying OpenSSL runtime DLLs next to ${t}"
        )
    endif()
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()
