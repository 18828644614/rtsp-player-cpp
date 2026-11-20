cmake_minimum_required(VERSION 3.16)
project(rtsp_player_cpp LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This player scaffold targets Windows.")
endif()

add_executable(rtsp_player src/main.cpp)
target_compile_features(rtsp_player PRIVATE cxx_std_17)
set_target_properties(rtsp_player PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(rtsp_player PRIVATE /W4 /utf-8)
else()
    target_compile_options(rtsp_player PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME player_help COMMAND rtsp_player --help)
    set_tests_properties(player_help PROPERTIES
        PASS_REGULAR_EXPRESSION "RTSP transport is not implemented yet" TIMEOUT 5)
endif()
