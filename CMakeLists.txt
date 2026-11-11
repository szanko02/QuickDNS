cmake_minimum_required(VERSION 3.20)
project(QuickDNS VERSION 2.0.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Unicode, UTF-8 charset, and warning flags
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-finput-charset=utf-8 -fexec-charset=utf-8 -Wall -Wextra -Wpedantic)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Common Windows System Libraries
if(MSVC)
    set(WIN32_LIBS
        iphlpapi.lib
        dnsapi.lib
        ws2_32.lib
        winhttp.lib
        comctl32.lib
        uxtheme.lib
        dwmapi.lib
        shell32.lib
        user32.lib
        gdi32.lib
        advapi32.lib
        ole32.lib
        oleaut32.lib
        uuid.lib
        gdiplus.lib
        bcrypt.lib
    )
else()
    set(WIN32_LIBS
        iphlpapi
        dnsapi
        ws2_32
        winhttp
        comctl32
        uxtheme
        dwmapi
        shell32
        user32
        gdi32
        advapi32
        ole32
        oleaut32
        uuid
        gdiplus
        bcrypt
    )
endif()

# Static library for core logic (reusable in tests)
add_library(quickdns_core STATIC
    src/dns_manager.cpp
    src/dns_benchmark.cpp
    src/config_manager.cpp
    src/smartdns_verifier.cpp
    src/app_updater.cpp
)
target_include_directories(quickdns_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)
target_link_libraries(quickdns_core PUBLIC ${WIN32_LIBS})

# GUI Application
set(APP_SOURCES
    src/main.cpp
    src/ui_main.cpp
    src/dialog_add_profile.cpp
    src/dialog_benchmark.cpp
    src/dialog_smartdns.cpp
    res/resource.rc
)

add_executable(QuickDNS WIN32 ${APP_SOURCES})
target_include_directories(QuickDNS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)
target_link_libraries(QuickDNS PRIVATE quickdns_core ${WIN32_LIBS})

if(MSVC)
    # Avoid duplicate manifest embedding conflict between resource.rc and MSVC default manifest
    set_target_properties(QuickDNS PROPERTIES LINK_FLAGS "/MANIFEST:NO")
else()
    target_link_options(QuickDNS PRIVATE -municode)
endif()

# Diagnostic resources for test tools
set(DIAG_RC res/resource_diag.rc)

# Test targets
enable_testing()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dns.cpp")
    add_executable(test_dns tests/test_dns.cpp ${DIAG_RC})
    target_link_libraries(test_dns PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestDns COMMAND test_dns)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_config.cpp")
    add_executable(test_config tests/test_config.cpp ${DIAG_RC})
    target_link_libraries(test_config PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestConfig COMMAND test_config)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark.cpp")
    add_executable(test_benchmark tests/test_benchmark.cpp ${DIAG_RC})
    target_link_libraries(test_benchmark PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestBenchmark COMMAND test_benchmark)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_smartdns_verifier.cpp")
    add_executable(test_smartdns_verifier tests/test_smartdns_verifier.cpp ${DIAG_RC})
    target_link_libraries(test_smartdns_verifier PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestSmartDnsVerifier COMMAND test_smartdns_verifier)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_updater.cpp")
    add_executable(test_updater tests/test_updater.cpp ${DIAG_RC})
    target_link_libraries(test_updater PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestUpdater COMMAND test_updater)
endif()

# Mark tests that perform live network/DNS/HTTP operations
set_tests_properties(TestBenchmark TestSmartDnsVerifier TestUpdater PROPERTIES LABELS "network")
set_tests_properties(TestDns TestConfig PROPERTIES TIMEOUT 30)
