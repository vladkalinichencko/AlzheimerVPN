cmake_minimum_required(VERSION 3.21)
project(AgwKeyTests LANGUAGES NONE)

set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_program(PYTHON3 NAMES python3)

if(NOT PYTHON3)
    message(STATUS "python3 not found; skipping AGW key check")
    return()
endif()

enable_testing()
add_test(NAME agw_key
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=
        ${PYTHON3} "${CMAKE_CURRENT_SOURCE_DIR}/check_agw_key.py")
