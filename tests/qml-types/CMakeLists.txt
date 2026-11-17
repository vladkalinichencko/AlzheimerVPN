cmake_minimum_required(VERSION 3.21)
project(QmlTypesTests LANGUAGES NONE)

set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_program(QMLLINT NAMES qmllint qmllint6
    HINTS /opt/homebrew/bin /opt/homebrew/opt/qt/bin /usr/local/bin)

if(NOT QMLLINT)
    message(STATUS "qmllint not found; skipping QML type checks")
    return()
endif()

enable_testing()
add_test(NAME qml_types
    COMMAND ${CMAKE_COMMAND} -E env QMLLINT=${QMLLINT}
        python3 "${CMAKE_CURRENT_SOURCE_DIR}/check_qml_types.py" "${REPO}")
