cmake_minimum_required(VERSION 3.21)
project(XrayLifecycleTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Test Network Qml Core5Compat)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(xray_lifecycle main.cpp stubs/core/utils/ipcClient.h
    "${REPO}/client/core/protocols/xrayProtocol.cpp"
    "${REPO}/client/core/protocols/vpnProtocol.h"
    "${REPO}/client/core/utils/errorCodes.h"
    "${REPO}/client/core/utils/networkUtilities.cpp"
    "${REPO}/client/core/utils/splitTunnelRule.cpp")
target_include_directories(xray_lifecycle PRIVATE stubs "${REPO}/client" "${REPO}/ipc")
target_compile_definitions(xray_lifecycle PRIVATE DEV_AGW_ENDPOINT="http://127.0.0.1/")
target_link_libraries(xray_lifecycle PRIVATE Qt6::Test Qt6::Network Qt6::Qml Qt6::Core5Compat)
enable_testing()
add_test(NAME xray_lifecycle COMMAND xray_lifecycle)
