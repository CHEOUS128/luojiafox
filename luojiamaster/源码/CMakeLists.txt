cmake_minimum_required(VERSION 3.12)
project(luojiamaster)

# set options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# fix issues in MSVC
if(MSVC)
    add_compile_options("/utf-8")
endif()

# third party
include(FetchContent)

set(CMAKE_SKIP_INSTALL_RULES ON) # we can't use an option to skip async_mqtt install rules, which is failed because some boost targets are not export
set(BOOST_ENABLE_COMPATIBILITY_TARGETS ON)
set(ASYNC_MQTT_USE_STATIC_BOOST ON)
set(ASYNC_MQTT_BUILD_LIB ON)
set(protobuf_INSTALL OFF)
set(protobuf_BUILD_TESTS OFF)
set(protobuf_BUILD_PROTOC_BINARIES OFF)
set(protobuf_BUILD_LIBPROTOC OFF)
set(protobuf_BUILD_LIBUPB OFF)
set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
set(protobuf_BUILD_SHARED_LIBS OFF)
set(protobuf_BUILD_PROTOBUF_LIBRARIES OFF)
set(protobuf_WITH_ZLIB OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
set(GLFW_LIBRARY_TYPE STATIC)

if(WIN32)
    FetchContent_Declare(
            directxtk12
            URL https://github.com/microsoft/DirectXTK12/archive/refs/tags/may2026.tar.gz
            URL_HASH SHA256=665af51a84243001cce1000265ec638e81b56d323516314689a91c3ea41f91b5
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/directxtk12"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
else ()
    FetchContent_Declare(
            glfw
            URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
            URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/glfw"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()
FetchContent_Declare(
    abseil-cpp
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG fe126c727b925b7c3b2742f6f54690973216952b # May 29, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/abseil-cpp"
)
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/spdlog"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
    URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/boost"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
    async_mqtt
    URL https://github.com/redboltz/async_mqtt/archive/refs/tags/10.3.0.tar.gz
    URL_HASH SHA256=01daa741f970a04551c8375553bc96ce66a0bf542466a88a141863cc8c2a690c
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/async_mqtt"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG 131af2c479c6ba36142ee9805e9f60fc4adfa22c # Mar 29, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/readerwriterqueue"
)
FetchContent_Declare(
    protobuf
    URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v35.0.tar.gz
    URL_HASH SHA256=e127ea69dd7be4e88abdd95845fb6c30d25d96971d95827e92b70e2e910d46a1
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/protobuf"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    stb_src
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4 # Apr 16, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/stb"
)
FetchContent_Declare(
    imgui_src
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    URL_HASH SHA256=fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(WIN32)
    FetchContent_MakeAvailable(directxtk12)
else ()
    FetchContent_MakeAvailable(glfw)
endif()
FetchContent_MakeAvailable(abseil-cpp spdlog boost async_mqtt readerwriterqueue protobuf imgui_src stb_src)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/stb)
add_library(imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/misc/cpp/imgui_stdlib.cpp
)
target_compile_definitions(imgui PUBLIC
    IMGUI_USER_CONFIG="my_imgui_config.h"
)
if(WIN32)
    target_sources(imgui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_dx12.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_win32.cpp
    )
else()
    target_sources(imgui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_vulkan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_glfw.cpp
    )
endif()
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/misc/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/include
)
if(WIN32)
    target_link_libraries(imgui PUBLIC
        d3d12.lib dxgi.lib
        DirectXTK12
    )
else()
    target_link_libraries(imgui PUBLIC
        glfw
        vulkan
    )
endif()
add_subdirectory(3rdparty/ffmpeg)

set(GENERATED_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/resource")
file(MAKE_DIRECTORY "${GENERATED_RESOURCE_DIR}")

function(write_file_if_different output_path content)
    set(temp_file "${output_path}.tmp")
    file(WRITE "${temp_file}" "${content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${temp_file}" "${output_path}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    file(REMOVE "${temp_file}")
endfunction()

function(add_embedded_resource input_relative_path generated_name)
    set(input_file "${CMAKE_CURRENT_SOURCE_DIR}/${input_relative_path}")
    set(output_header "${GENERATED_RESOURCE_DIR}/${generated_name}.h")
    set(output_source "${GENERATED_RESOURCE_DIR}/${generated_name}.cpp")

    file(SIZE "${input_file}" embedded_resource_size)
    file(READ "${input_file}" embedded_resource_hex HEX)
    string(TOLOWER "${embedded_resource_hex}" embedded_resource_hex)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " embedded_resource_bytes "${embedded_resource_hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){32})" "\\1\n    " embedded_resource_bytes "${embedded_resource_bytes}")

    string(CONCAT output_header_content
        "#pragma once\n\n"
        "extern const unsigned char ${generated_name}_data[];\n"
        "extern const unsigned int ${generated_name}_data_len;\n"
    )
    write_file_if_different("${output_header}" "${output_header_content}")

    string(CONCAT output_source_content
        "#include \"${generated_name}.h\"\n\n"
        "const unsigned char ${generated_name}_data[] = {\n"
        "    ${embedded_resource_bytes}\n"
        "};\n\n"
        "const unsigned int ${generated_name}_data_len = ${embedded_resource_size}u;\n"
    )
    write_file_if_different("${output_source}" "${output_source_content}")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${input_file}"
    )
    set(EMBEDDED_RESOURCE_SOURCES ${EMBEDDED_RESOURCE_SOURCES} "${output_source}" PARENT_SCOPE)
endfunction()

set(EMBEDDED_RESOURCE_SOURCES "")
add_embedded_resource("resources/map.png" "embedded_map_png")
add_embedded_resource("resources/zcool.ttf" "embedded_zcool_font")
add_embedded_resource("proto/messages.proto" "embedded_messages_proto")

# add executable
add_executable(luojiamaster WIN32
    src/main.cpp
    src/app/application.cpp
    src/app/imgui_setup.cpp
    src/custom_control_payload.cpp
    src/main_window.cpp
    src/keyboard_mouse_state_manager.cpp
    src/panel/bottom_panel.cpp
    src/panel/buff_panel.cpp
    src/panel/crosshair.cpp
    src/panel/custom_video_panel.cpp
    src/panel/header_panel.cpp
    src/panel/log_panel.cpp
    src/panel/mini_map.cpp
    src/panel/panel_layout.cpp
    src/panel/robot_status_panel.cpp
    src/panel/start_panel.cpp
    src/logging/logging.cpp
    src/io/mqtt_client.cpp
    src/io/video_stream_receiver.cpp
    src/video/custom_byte_block_video_decoder.cpp
    src/video/custom_byte_block_video_decoder_worker.cpp
    src/video/rgba_image_transform.cpp
    src/video/video_decoder.cpp
    src/video/video_shards_manager.cpp
    src/video/video_stream_manager.cpp
    src/platform/linux_platform_backend.cpp
    src/platform/windows_platform_backend.cpp
    src/resource/resources.cpp
    gen/messages.pb.cc
    ${EMBEDDED_RESOURCE_SOURCES}
    resources/resources.rc
)
if(WIN32)
    target_sources(luojiamaster PRIVATE resources/resources.rc)
endif()
target_precompile_headers(luojiamaster PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/pch.h>")
target_include_directories(luojiamaster PRIVATE
    include
    gen
    src
    ${GENERATED_RESOURCE_DIR}
)

if(WIN32)
    target_compile_definitions(luojiamaster PRIVATE _WIN32_WINNT=0x0601)
endif()
target_compile_definitions(luojiamaster PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

set_source_files_properties(
    src/resource/resources.cpp
    ${EMBEDDED_RESOURCE_SOURCES}
    PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(luojiamaster PRIVATE
    spdlog::spdlog
    utf8_validity
    protobuf::libprotobuf-lite
    async_mqtt_asio_bind
    ffmpeg
    imgui
    readerwriterqueue
    stb
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(luojiamaster PRIVATE -Wall -Wextra)
    set_source_files_properties(
        ${EMBEDDED_RESOURCE_SOURCES}
        PROPERTIES
        COMPILE_FLAGS -g0
    )
elseif(MSVC)
    target_compile_options(luojiamaster PRIVATE /bigobj /W4)
    target_link_options(luojiamaster PRIVATE /MAP)
endif()
