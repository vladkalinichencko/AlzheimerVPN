cmake_minimum_required(VERSION 3.25.0)

project(AmneziaVPN_Tests)

find_package(Qt6 REQUIRED COMPONENTS Test)


set(CMAKE_AUTORCC ON)

qt6_add_resources(TEST_QRC
    ${CLIENT_ROOT_DIR}/server_scripts/serverScripts.qrc
)

add_library(test_common OBJECT
    ${SOURCES}
    ${HEADERS}
    ${TEST_QRC}
)

qt_add_repc_replicas(test_common
    ${CLIENT_ROOT_DIR}/../ipc/ipc_interface.rep
    ${CLIENT_ROOT_DIR}/../ipc/ipc_process_interface.rep
)

target_link_libraries(test_common PUBLIC
    ${LIBS}
)

target_include_directories(test_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(test_split_tunnel_rule
    testSplitTunnelRule.cpp
)

target_link_libraries(test_split_tunnel_rule PRIVATE
    Qt6::Test
    test_common
)

add_executable(test_dns_message_parser
    testDnsMessageParser.cpp
)

target_link_libraries(test_dns_message_parser PRIVATE
    Qt6::Test
    test_common
)

add_executable(test_split_tunnel_route_planner
    testSplitTunnelRoutePlanner.cpp
)

target_link_libraries(test_split_tunnel_route_planner PRIVATE
    Qt6::Test
    test_common
)

add_executable(test_connection_health
    testConnectionHealth.cpp
)

target_link_libraries(test_connection_health PRIVATE
    Qt6::Test
    test_common
)

add_executable(test_apple_native_stack
    testAppleNativeStack.cpp
)

target_compile_definitions(test_apple_native_stack PRIVATE
    AMNEZIA_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_apple_native_stack PRIVATE
    Qt6::Test
)

enable_testing()
add_test(NAME SplitTunnelRuleTest COMMAND test_split_tunnel_rule)
add_test(NAME DnsMessageParserTest COMMAND test_dns_message_parser)
add_test(NAME SplitTunnelRoutePlannerTest COMMAND test_split_tunnel_route_planner)
add_test(NAME ConnectionHealthTest COMMAND test_connection_health)
add_test(NAME AppleNativeStackTest COMMAND test_apple_native_stack)
