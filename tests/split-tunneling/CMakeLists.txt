cmake_minimum_required(VERSION 3.21)
project(SplitTunnelingTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Test Network Core5Compat Quick QuickControls2)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(split_tunneling
    main.cpp settingsStub.cpp
    "${REPO}/client/secureQSettings.h"
    "${REPO}/client/core/repositories/secureAppSettingsRepository.cpp"
    "${REPO}/client/core/controllers/ipSplitTunnelingController.cpp"
    "${REPO}/client/core/utils/splitTunnelRule.cpp"
    "${REPO}/client/core/utils/routeModes.h"
    "${REPO}/client/core/utils/networkUtilities.cpp"
    "${REPO}/client/ui/models/ipSplitTunnelingModel.cpp")
target_include_directories(split_tunneling PRIVATE "${REPO}/client")
target_compile_definitions(split_tunneling PRIVATE QTKEYCHAIN_NO_EXPORT DEV_AGW_ENDPOINT="http://127.0.0.1/")
target_link_libraries(split_tunneling PRIVATE Qt6::Test Qt6::Network Qt6::Core5Compat)
enable_testing()
add_test(NAME split_tunneling COMMAND split_tunneling)

add_executable(split_preview preview.cpp "${REPO}/client/images/images.qrc")
target_link_libraries(split_preview PRIVATE Qt6::Quick Qt6::QuickControls2)
