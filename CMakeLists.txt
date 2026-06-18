cmake_minimum_required(VERSION 3.16)
project(argus C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
pkg_check_modules(ONNXRUNTIME REQUIRED IMPORTED_TARGET libonnxruntime)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

set(ARGUS_LOCAL_LIBDATACHANNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel")
set(ARGUS_PARENT_LIBDATACHANNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libdatachannel")

if(EXISTS "${ARGUS_LOCAL_LIBDATACHANNEL_DIR}/CMakeLists.txt")
    add_subdirectory(
        "${ARGUS_LOCAL_LIBDATACHANNEL_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel"
    )
    set(ARGUS_JSON_INCLUDE_DIR
        "${ARGUS_LOCAL_LIBDATACHANNEL_DIR}/deps/json/single_include"
    )
elseif(EXISTS "${ARGUS_PARENT_LIBDATACHANNEL_DIR}/CMakeLists.txt")
    message(WARNING
        "Using ../libdatachannel fallback. For standalone argus, vendor it into "
        "argus/third_party/libdatachannel (tools/vendor_libdatachannel.sh)."
    )
    add_subdirectory(
        "${ARGUS_PARENT_LIBDATACHANNEL_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/libdatachannel"
    )
    set(ARGUS_JSON_INCLUDE_DIR
        "${ARGUS_PARENT_LIBDATACHANNEL_DIR}/deps/json/single_include"
    )
else()
    message(FATAL_ERROR
        "Missing libdatachannel. Expected one of:\n"
        "  ${ARGUS_LOCAL_LIBDATACHANNEL_DIR}\n"
        "  ${ARGUS_PARENT_LIBDATACHANNEL_DIR}\n"
        "Run tools/vendor_libdatachannel.sh or copy libdatachannel into third_party."
    )
endif()

add_executable(argus
    cmd/argus/main.cpp
    core/compose/compose_handle.cpp
    core/compose/compose_type.cpp
    core/compose/common/jpg_writer.cpp
    core/compose/jpg_snapshot/jpg_snapshot_compose.cpp
    core/compose/side_by_side/side_by_side_compose.cpp
    core/compose/side_by_side/side_by_side_frame.cpp
    core/compose/clip_prompt/clip_prompt_compose.cpp
    core/compose/clip_prompt/clip_tokenizer.cpp
    core/compose/clip_prompt/model_path.cpp
    core/compose/clip_prompt/mobileclip2_s2_engine.cpp
    core/decoder/decoder_handle.cpp
    core/decoder/decoder_runner.cpp
    core/decoder/video_frame_utils.cpp
    core/decoder/h264_decoder.cpp
    core/decoder/packet_adapter.cpp
    core/runtime/runtime_http.cpp
    core/runtime/runtime_manifest.cpp
    core/runtime/runtime.cpp
    core/runtime/sink_handle.cpp
    core/servers/rtmp/rtmp_amf0.c
    core/servers/rtmp/rtmp_chunk.c
    core/servers/rtmp/rtmp_handshake.c
    core/servers/rtmp/rtmp_server.cpp
    core/servers/rtmp/rtmp_session.cpp
    core/servers/webrtc/webrtc_server.cpp
    core/sinks/hls_sink.cpp
    core/sinks/hls_sink_runner.cpp
)

add_executable(argus_runtime_manifest_test
    core/tests/runtime_manifest_test.cpp
    core/compose/compose_type.cpp
    core/runtime/runtime_manifest.cpp
)

add_executable(argus_model_path_test
    core/tests/model_path_test.cpp
    core/compose/clip_prompt/model_path.cpp
)

add_executable(argus_clip_tokenizer_test
    core/tests/clip_tokenizer_test.cpp
    core/compose/clip_prompt/clip_tokenizer.cpp
)

add_executable(argus_side_by_side_frame_test
    core/tests/side_by_side_frame_test.cpp
    core/compose/side_by_side/side_by_side_frame.cpp
)

add_executable(argus_packet_adapter_test
    core/tests/packet_adapter_test.cpp
    core/decoder/packet_adapter.cpp
)

target_include_directories(argus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ARGUS_JSON_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIRS}
)

target_include_directories(argus_runtime_manifest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(argus_packet_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(argus_model_path_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(argus_clip_tokenizer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ARGUS_JSON_INCLUDE_DIR}
)

target_include_directories(argus_side_by_side_frame_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(argus PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(argus_runtime_manifest_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(argus_packet_adapter_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(argus_model_path_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(argus_clip_tokenizer_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(argus_side_by_side_frame_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(argus PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::ONNXRUNTIME
    LibDataChannel::LibDataChannelStatic
    Threads::Threads
)

target_link_libraries(argus_runtime_manifest_test PRIVATE
    Threads::Threads
)

target_link_libraries(argus_packet_adapter_test PRIVATE
    Threads::Threads
)

target_link_libraries(argus_model_path_test PRIVATE
    Threads::Threads
)

target_link_libraries(argus_clip_tokenizer_test PRIVATE
    Threads::Threads
)

target_link_libraries(argus_side_by_side_frame_test PRIVATE
    PkgConfig::FFMPEG
    Threads::Threads
)

enable_testing()
add_test(NAME runtime_manifest_test COMMAND argus_runtime_manifest_test)
add_test(NAME packet_adapter_test COMMAND argus_packet_adapter_test)
add_test(NAME model_path_test COMMAND argus_model_path_test)
set_tests_properties(model_path_test PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME clip_tokenizer_test COMMAND argus_clip_tokenizer_test)
set_tests_properties(clip_tokenizer_test PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME side_by_side_frame_test COMMAND argus_side_by_side_frame_test)
set_tests_properties(side_by_side_frame_test PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
