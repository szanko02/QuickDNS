cmake_minimum_required(VERSION 3.20)
project(QuickDNS VERSION 2.3.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Unicode, UTF-8 charset, and compiler warning flags
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/utf-8 /W4 /permissive-)
else()
    add_compile_options(-finput-charset=utf-8 -fexec-charset=utf-8 -Wall -Wextra -Wno-unknown-pragmas)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Core Win32 system libraries (shared with GUI and console test targets)
set(CORE_WIN32_LIBS
    secur32
    crypt32
    bcrypt
    winhttp
    iphlpapi
    dnsapi
    ws2_32
    advapi32
    shell32
    user32
    ole32
    uuid
)

# GUI-only Win32 libraries (place gdiplus BEFORE gdi32/ole32/user32 for single-pass GNU ld)
set(GUI_WIN32_LIBS
    comctl32
    uxtheme
    dwmapi
    gdiplus
    gdi32
    oleaut32
    ole32
    user32
    uuid
)

# Static library for core logic (reusable across GUI and unit tests)
add_library(quickdns_core STATIC
    src/dns_manager.cpp
    src/dns_benchmark.cpp
    src/config_manager.cpp
    src/smartdns_verifier.cpp
    src/app_updater.cpp
    src/ai_checker.cpp
)
target_include_directories(quickdns_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)
target_compile_definitions(quickdns_core PUBLIC
    OVERRIDE_APP_VERSION="${PROJECT_VERSION}"
    OVERRIDE_APP_VERSION_W=L"${PROJECT_VERSION}"
)
target_link_libraries(quickdns_core PUBLIC ${CORE_WIN32_LIBS})

# GUI Application
set(APP_SOURCES
    src/main.cpp
    src/ui_main.cpp
    src/dialog_add_profile.cpp
    src/dialog_benchmark.cpp
    src/dialog_smartdns.cpp
    src/dialog_ai_check.cpp
    res/resource.rc
)

add_executable(QuickDNS WIN32 ${APP_SOURCES})
target_include_directories(QuickDNS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)
target_link_libraries(QuickDNS PRIVATE quickdns_core ${GUI_WIN32_LIBS})

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Avoid duplicate manifest embedding conflict between resource.rc and MSVC default manifest
    target_link_options(QuickDNS PRIVATE /MANIFEST:NO)
elseif(MSVC)
    target_link_options(QuickDNS PRIVATE "LINKER:/MANIFEST:NO")
else()
    target_link_options(QuickDNS PRIVATE -municode)
endif()

# Diagnostic resources for test executables
set(DIAG_RC res/resource_diag.rc)

# Test targets & CTest suites
enable_testing()

add_executable(test_dns tests/test_dns.cpp ${DIAG_RC})
target_link_libraries(test_dns PRIVATE quickdns_core)
add_test(NAME TestDnsOffline COMMAND test_dns --offline WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME TestDnsNetwork COMMAND test_dns --network WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_config tests/test_config.cpp ${DIAG_RC})
target_link_libraries(test_config PRIVATE quickdns_core)
target_compile_definitions(test_config PRIVATE QUICKDNS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME TestConfig COMMAND test_config WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_benchmark tests/test_benchmark.cpp ${DIAG_RC})
target_link_libraries(test_benchmark PRIVATE quickdns_core)
add_test(NAME TestBenchmarkOffline COMMAND test_benchmark --offline WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME TestBenchmarkNetwork COMMAND test_benchmark --network WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_smartdns_verifier tests/test_smartdns_verifier.cpp ${DIAG_RC})
target_link_libraries(test_smartdns_verifier PRIVATE quickdns_core)
add_test(NAME TestSmartDnsVerifierOffline COMMAND test_smartdns_verifier --offline WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME TestSmartDnsVerifierNetwork COMMAND test_smartdns_verifier --network WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_updater tests/test_updater.cpp ${DIAG_RC})
target_link_libraries(test_updater PRIVATE quickdns_core)
add_test(NAME TestUpdaterOffline COMMAND test_updater --offline WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME TestUpdaterNetwork COMMAND test_updater --network WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_ai_checker tests/test_ai_checker.cpp ${DIAG_RC})
target_link_libraries(test_ai_checker PRIVATE quickdns_core)
add_test(NAME TestAiChecker COMMAND test_ai_checker WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Deterministic offline suites (executed by `ctest -C Release --output-on-failure -LE network`)
set_tests_properties(
    TestDnsOffline
    TestConfig
    TestBenchmarkOffline
    TestSmartDnsVerifierOffline
    TestUpdaterOffline
    TestAiChecker
    PROPERTIES TIMEOUT 30
)

# Live network/DNS/HTTP integration suites
set_tests_properties(
    TestDnsNetwork
    TestBenchmarkNetwork
    TestSmartDnsVerifierNetwork
    TestUpdaterNetwork
    PROPERTIES LABELS "network" TIMEOUT 120
)
