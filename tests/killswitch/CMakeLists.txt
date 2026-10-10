cmake_minimum_required(VERSION 3.21)
project(ConnectionKillSwitchTests LANGUAGES CXX)
if(NOT APPLE)
    message(FATAL_ERROR "These tests exercise the macOS kill switch")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Core5Compat)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define ORGANIZATION_NAME \"KillSwitchTest\"\n#define APPLICATION_NAME \"KillSwitchTest\"\n")
add_executable(connection_killswitch
    main.cpp
    "${REPO}/service/server/killswitch.cpp"
    "${REPO}/service/server/killswitch.h"
    "${REPO}/client/secureQSettings.h")
target_include_directories(connection_killswitch PRIVATE "${REPO}/client" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(connection_killswitch PRIVATE QTKEYCHAIN_NO_EXPORT)
target_link_libraries(connection_killswitch PRIVATE Qt6::Widgets Qt6::Network Qt6::Core5Compat)
enable_testing()
add_test(NAME connection_killswitch COMMAND connection_killswitch)
