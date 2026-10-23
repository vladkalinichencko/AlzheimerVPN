cmake_minimum_required(VERSION 3.21)
project(status_icon LANGUAGES CXX OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
enable_testing()
add_executable(status_icon main.mm
    ../../client/platforms/macos/macosstatusicon.mm
    ../../client/platforms/macos/macosstatusicon.h
    ../../client/images/images.qrc)
target_include_directories(status_icon PRIVATE ../../client ../../client/mozilla/shared)
target_link_libraries(status_icon PRIVATE Qt6::Widgets "-framework Cocoa" "-framework UserNotifications")
add_test(NAME status_icon COMMAND status_icon)
