cmake_minimum_required(VERSION 3.20)
project(QuickDNS VERSION 2.1.0 LANGUAGES C CXX)

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
        secur32.lib
        crypt32.lib
        comdlg32.lib
        runtimeobject.lib
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
        secur32
        crypt32
        comdlg32
        runtimeobject
    )
endif()

# Static library for core logic (reusable in tests)
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
target_link_libraries(quickdns_core PUBLIC ${WIN32_LIBS})

# GUI Application
set(APP_SOURCES
    src/main.cpp
    src/ui_main.cpp
    src/dialog_add_profile.cpp
    src/dialog_benchmark.cpp
    src/dialog_smartdns.cpp
    src/dialog_ai_check.cpp
    src/dialog_app_routing.cpp
    src/routing_installed_packages.cpp
    src/app_routing_config.cpp
    src/routing_adapters.cpp
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

add_executable(test_primary_adapter_metric tests/test_primary_adapter_metric.cpp src/primary_adapter_metric.cpp)
target_include_directories(test_primary_adapter_metric PRIVATE include)
add_test(NAME TestPrimaryAdapterMetric COMMAND test_primary_adapter_metric)

add_executable(test_primary_adapter_metric_windows tests/test_primary_adapter_metric_windows.cpp src/primary_adapter_metric_windows.cpp)
target_include_directories(test_primary_adapter_metric_windows PRIVATE include)
target_link_libraries(test_primary_adapter_metric_windows PRIVATE iphlpapi ws2_32)
add_test(NAME TestPrimaryAdapterMetricWindows COMMAND test_primary_adapter_metric_windows)

add_executable(test_primary_adapter_metric_journal tests/test_primary_adapter_metric_journal.cpp src/primary_adapter_metric_journal.cpp)
target_include_directories(test_primary_adapter_metric_journal PRIVATE include third_party)
add_test(NAME TestPrimaryAdapterMetricJournal COMMAND test_primary_adapter_metric_journal)

add_executable(test_routing_owner_table tests/test_routing_owner_table.cpp src/routing_owner_table.cpp)
target_include_directories(test_routing_owner_table PRIVATE include)
target_link_libraries(test_routing_owner_table PRIVATE iphlpapi ws2_32)
add_test(NAME TestRoutingOwnerTable COMMAND test_routing_owner_table)

add_executable(test_routing_owner_registry tests/test_routing_owner_registry.cpp src/routing_owner_registry.cpp src/routing_owner_table.cpp src/routing_flow_registry.cpp)
target_include_directories(test_routing_owner_registry PRIVATE include)
target_link_libraries(test_routing_owner_registry PRIVATE iphlpapi ws2_32)
add_test(NAME TestRoutingOwnerRegistry COMMAND test_routing_owner_registry)

add_executable(test_app_policy_lifecycle tests/test_app_policy_lifecycle.cpp src/app_policy_lifecycle.cpp src/app_policy_filter_identity.cpp src/app_connection_policy.cpp)
target_include_directories(test_app_policy_lifecycle PRIVATE include)
target_link_libraries(test_app_policy_lifecycle PRIVATE fwpuclnt advapi32)
add_test(NAME TestAppPolicyLifecycle COMMAND test_app_policy_lifecycle)

add_executable(test_app_policy_filter_identity tests/test_app_policy_filter_identity.cpp src/app_policy_filter_identity.cpp)
target_include_directories(test_app_policy_filter_identity PRIVATE include)
target_link_libraries(test_app_policy_filter_identity PRIVATE fwpuclnt advapi32)
add_test(NAME TestAppPolicyFilterIdentity COMMAND test_app_policy_filter_identity)

add_executable(test_main_ui_routing tests/test_main_ui_routing.cpp src/ui_main.cpp
    src/dialog_add_profile.cpp src/dialog_benchmark.cpp src/dialog_smartdns.cpp src/dialog_ai_check.cpp
    src/dialog_app_routing.cpp src/routing_installed_packages.cpp src/app_routing_config.cpp src/routing_adapters.cpp ${DIAG_RC})
target_link_libraries(test_main_ui_routing PRIVATE quickdns_core ${WIN32_LIBS})
add_test(NAME TestMainUiRouting COMMAND test_main_ui_routing)
set_tests_properties(TestMainUiRouting PROPERTIES TIMEOUT 15)

add_executable(test_app_guard_enable tests/test_app_guard_enable.cpp src/app_guard_enable.cpp src/app_guard_journal.cpp src/app_route_guard.cpp)
target_include_directories(test_app_guard_enable PRIVATE include third_party)
target_link_libraries(test_app_guard_enable PRIVATE ole32 fwpuclnt)
add_test(NAME TestAppGuardEnable COMMAND test_app_guard_enable)

add_executable(test_app_guard_recovery tests/test_app_guard_recovery.cpp src/app_guard_recovery.cpp src/app_guard_journal.cpp src/app_route_guard.cpp)
target_include_directories(test_app_guard_recovery PRIVATE include third_party)
target_link_libraries(test_app_guard_recovery PRIVATE ole32 fwpuclnt)
add_test(NAME TestAppGuardRecovery COMMAND test_app_guard_recovery)

add_executable(test_app_guard_journal tests/test_app_guard_journal.cpp src/app_guard_journal.cpp)
target_include_directories(test_app_guard_journal PRIVATE include third_party)
target_link_libraries(test_app_guard_journal PRIVATE ole32)
add_test(NAME TestAppGuardJournal COMMAND test_app_guard_journal)

add_executable(test_dialog_app_routing tests/test_dialog_app_routing.cpp src/dialog_app_routing.cpp src/routing_installed_packages.cpp src/app_routing_config.cpp)
target_include_directories(test_dialog_app_routing PRIVATE include third_party)
target_link_libraries(test_dialog_app_routing PRIVATE ${WIN32_LIBS} comdlg32 runtimeobject)
add_test(NAME TestDialogAppRouting COMMAND test_dialog_app_routing)

add_executable(test_routing_packet_policy tests/test_routing_packet_policy.cpp src/routing_packet_policy.cpp)
target_include_directories(test_routing_packet_policy PRIVATE include)
add_test(NAME TestRoutingPacketPolicy COMMAND test_routing_packet_policy)

add_executable(test_routing_flow_registry tests/test_routing_flow_registry.cpp src/routing_flow_registry.cpp)
target_include_directories(test_routing_flow_registry PRIVATE include)
add_test(NAME TestRoutingFlowRegistry COMMAND test_routing_flow_registry)

add_library(fake_windivert SHARED tests/fake_windivert.cpp)
add_executable(test_routing_divert_module tests/test_routing_divert_module.cpp src/routing_divert_module.cpp)
target_include_directories(test_routing_divert_module PRIVATE include third_party)
add_dependencies(test_routing_divert_module fake_windivert)
add_test(NAME TestRoutingDivertModule COMMAND test_routing_divert_module $<TARGET_FILE:fake_windivert>)

add_executable(test_routing_socket_events tests/test_routing_socket_events.cpp src/routing_socket_events.cpp)
target_include_directories(test_routing_socket_events PRIVATE include third_party)
add_test(NAME TestRoutingSocketEvents COMMAND test_routing_socket_events)

add_executable(test_routing_network_capture tests/test_routing_network_capture.cpp src/routing_network_capture.cpp)
target_include_directories(test_routing_network_capture PRIVATE include third_party)
add_test(NAME TestRoutingNetworkCapture COMMAND test_routing_network_capture)
set_tests_properties(TestRoutingNetworkCapture PROPERTIES TIMEOUT 10)

add_executable(test_routing_nat tests/test_routing_nat.cpp src/routing_nat.cpp)
target_include_directories(test_routing_nat PRIVATE include)
add_test(NAME TestRoutingNat COMMAND test_routing_nat)

add_executable(test_routing_packet_processor tests/test_routing_packet_processor.cpp src/routing_packet_processor.cpp src/routing_flow_registry.cpp src/routing_packet_policy.cpp src/routing_nat.cpp)
target_include_directories(test_routing_packet_processor PRIVATE include)
add_test(NAME TestRoutingPacketProcessor COMMAND test_routing_packet_processor)

add_executable(test_app_route_guard tests/test_app_route_guard.cpp src/app_route_guard.cpp)
target_include_directories(test_app_route_guard PRIVATE include)
target_link_libraries(test_app_route_guard PRIVATE fwpuclnt)
add_test(NAME TestAppRouteGuard COMMAND test_app_route_guard)

add_executable(test_app_connection_policy tests/test_app_connection_policy.cpp src/app_connection_policy.cpp)
target_include_directories(test_app_connection_policy PRIVATE include)
target_link_libraries(test_app_connection_policy PRIVATE fwpuclnt)
add_test(NAME TestAppConnectionPolicy COMMAND test_app_connection_policy)

add_executable(test_routing_adapters tests/test_routing_adapters.cpp src/routing_adapters.cpp)
target_include_directories(test_routing_adapters PRIVATE include)
target_link_libraries(test_routing_adapters PRIVATE iphlpapi ws2_32)
add_test(NAME TestRoutingAdapters COMMAND test_routing_adapters)

add_executable(test_app_routing_config tests/test_app_routing_config.cpp src/app_routing_config.cpp)
target_include_directories(test_app_routing_config PRIVATE include third_party)
add_test(NAME TestAppRoutingConfig COMMAND test_app_routing_config)

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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ai_checker.cpp")
    add_executable(test_ai_checker tests/test_ai_checker.cpp ${DIAG_RC})
    target_link_libraries(test_ai_checker PRIVATE quickdns_core ${WIN32_LIBS})
    add_test(NAME TestAiChecker COMMAND test_ai_checker)
endif()

# Mark tests that perform live network/DNS/HTTP operations
set_tests_properties(TestBenchmark TestSmartDnsVerifier TestUpdater PROPERTIES LABELS "network")
set_tests_properties(TestDns TestConfig PROPERTIES TIMEOUT 30)
