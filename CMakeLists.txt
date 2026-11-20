cmake_minimum_required(VERSION 3.16)
project(rtsp_player_cpp LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This player targets Windows.")
endif()

# OBJECT 目标让程序和测试共用编译结果；运行程序只有 rtsp_player。
add_library(player_protocol OBJECT
    src/net/tcp_socket.cpp
    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_stream_parser.cpp
    src/rtsp/sdp.cpp
    src/rtsp/rtsp_client.cpp
    src/rtp/packet_stats.cpp)
target_include_directories(player_protocol PUBLIC src)
target_compile_features(player_protocol PUBLIC cxx_std_17)
target_compile_definitions(player_protocol PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(player_protocol PUBLIC ws2_32)
set_target_properties(player_protocol PROPERTIES CXX_EXTENSIONS OFF)

function(player_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
player_warnings(player_protocol)

add_executable(rtsp_player src/main.cpp)
target_link_libraries(rtsp_player PRIVATE player_protocol)
player_warnings(rtsp_player)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME player_help COMMAND rtsp_player --help)
    set_tests_properties(player_help PROPERTIES
        PASS_REGULAR_EXPRESSION "dual-track RTP/RTCP statistics" TIMEOUT 5)
    add_executable(protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(protocol_tests PRIVATE player_protocol)
    player_warnings(protocol_tests)
    add_test(NAME protocol COMMAND protocol_tests)
    set_tests_properties(protocol PROPERTIES TIMEOUT 5)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME session COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_tests.py"
            --player "$<TARGET_FILE:rtsp_player>")
        set_tests_properties(session PROPERTIES TIMEOUT 40)
    endif()
endif()
