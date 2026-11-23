include_guard(GLOBAL)

set(FFMPEG_ROOT "" CACHE PATH "FFmpeg shared SDK root (include/lib/bin)")
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES include)
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFmpeg headers missing. Set -DFFMPEG_ROOT=<shared SDK root>.")
endif()

foreach(component avcodec avutil)
    string(TOUPPER "${component}" component_upper)
    find_library(FFMPEG_${component_upper}_LIBRARY NAMES "${component}"
        HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES lib)
    if(NOT FFMPEG_${component_upper}_LIBRARY)
        message(FATAL_ERROR "FFmpeg ${component} import library missing. Set FFMPEG_ROOT.")
    endif()
    add_library(FFmpeg::${component} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${component} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_${component_upper}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
endforeach()

get_filename_component(ffmpeg_sdk_root "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
set(FFMPEG_RUNTIME_DIR "${ffmpeg_sdk_root}/bin" CACHE PATH "FFmpeg runtime DLL directory")
file(GLOB ffmpeg_runtime_dlls CONFIGURE_DEPENDS "${FFMPEG_RUNTIME_DIR}/*.dll")
if(NOT ffmpeg_runtime_dlls)
    message(FATAL_ERROR "FFmpeg DLLs missing. Set FFMPEG_RUNTIME_DIR to the SDK bin directory.")
endif()

# 正式程序和解码测试共用输出目录；只部署一次 SDK 自带的运行依赖。
function(copy_ffmpeg_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${ffmpeg_runtime_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()
