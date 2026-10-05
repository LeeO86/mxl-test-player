cmake_minimum_required(VERSION 3.24)
project(mxl-test-player VERSION 1.0.3 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PLAYER_BUILD_TESTS "Build unit tests" ON)
option(PLAYER_WITH_MXL "Build and link the pinned MXL SDK" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)
pkg_check_modules(FT REQUIRED IMPORTED_TARGET freetype2)
find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(PLAYER_WITH_MXL)
    set(MXL_TAG 218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7)
    FetchContent_Declare(mxl_src
        GIT_REPOSITORY https://github.com/dmf-mxl/mxl.git
        GIT_TAG ${MXL_TAG}
    )
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.1.4
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.1
        GIT_SHALLOW TRUE
    )
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(stduuid
        GIT_REPOSITORY https://github.com/mariusbancila/stduuid.git
        GIT_TAG v1.2.3
        GIT_SHALLOW TRUE
    )
    set(UUID_SYSTEM_GENERATOR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(picojson
        URL https://github.com/kazuho/picojson/archive/refs/tags/v1.3.0.tar.gz
    )
    FetchContent_MakeAvailable(fmt spdlog stduuid picojson)
    FetchContent_GetProperties(mxl_src)
    if(NOT mxl_src_POPULATED)
        FetchContent_Populate(mxl_src)
    endif()

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/pico_inc/picojson)
    file(CREATE_LINK ${picojson_SOURCE_DIR}/picojson.h ${CMAKE_BINARY_DIR}/pico_inc/picojson/picojson.h SYMBOLIC)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/pico_wrap/picojson)
    file(WRITE ${CMAKE_BINARY_DIR}/pico_wrap/picojson/wrapper.h
"#pragma once
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \"-Wmaybe-uninitialized\"
#include <picojson/picojson.h>
#pragma GCC diagnostic pop
")
    add_library(picojson_lib INTERFACE)
    target_include_directories(picojson_lib INTERFACE ${CMAKE_BINARY_DIR}/pico_wrap ${CMAKE_BINARY_DIR}/pico_inc)
    add_library(picojson::picojson ALIAS picojson_lib)

    file(WRITE ${CMAKE_BINARY_DIR}/mxl_version/mxl/version.h
"#pragma once
#define MXL_VERSION_MAJOR 1
#define MXL_VERSION_MINOR 1
#define MXL_VERSION_PATCH 0
#define MXL_VERSION_BUILD 0
#define MXL_VERSION_FULL \"1.1.0+0 g218ddaa0a08c\"
")

    set(MXL_INTERNAL ${mxl_src_SOURCE_DIR}/lib/internal)
    add_library(mxl STATIC
        ${mxl_src_SOURCE_DIR}/lib/src/flow.cpp
        ${mxl_src_SOURCE_DIR}/lib/src/mxl.cpp
        ${mxl_src_SOURCE_DIR}/lib/src/time.cpp
        ${MXL_INTERNAL}/src/DomainWatcher.cpp
        ${MXL_INTERNAL}/src/FlowData.cpp
        ${MXL_INTERNAL}/src/FlowInfo.cpp
        ${MXL_INTERNAL}/src/FlowIoFactory.cpp
        ${MXL_INTERNAL}/src/FlowManager.cpp
        ${MXL_INTERNAL}/src/FlowOptionsParser.cpp
        ${MXL_INTERNAL}/src/FlowParser.cpp
        ${MXL_INTERNAL}/src/FlowReader.cpp
        ${MXL_INTERNAL}/src/FlowSynchronizationGroup.cpp
        ${MXL_INTERNAL}/src/FlowWriter.cpp
        ${MXL_INTERNAL}/src/Instance.cpp
        ${MXL_INTERNAL}/src/Logging.cpp
        ${MXL_INTERNAL}/src/MediaUtils.cpp
        ${MXL_INTERNAL}/src/PathUtils.cpp
        ${MXL_INTERNAL}/src/PosixContinuousFlowReader.cpp
        ${MXL_INTERNAL}/src/PosixContinuousFlowWriter.cpp
        ${MXL_INTERNAL}/src/PosixDiscreteFlowReader.cpp
        ${MXL_INTERNAL}/src/PosixDiscreteFlowWriter.cpp
        ${MXL_INTERNAL}/src/PosixFlowIoFactory.cpp
        ${MXL_INTERNAL}/src/SharedMemory.cpp
        ${MXL_INTERNAL}/src/Sync.cpp
        ${MXL_INTERNAL}/src/Thread.cpp
        ${MXL_INTERNAL}/src/Time.cpp
        ${MXL_INTERNAL}/src/Timing.cpp
    )
    target_compile_features(mxl PUBLIC cxx_std_20)
    target_include_directories(mxl
        PUBLIC
            ${mxl_src_SOURCE_DIR}/lib/include
            ${CMAKE_BINARY_DIR}/mxl_version
        PRIVATE
            ${MXL_INTERNAL}/include
    )
    target_link_libraries(mxl PUBLIC fmt::fmt spdlog::spdlog stduuid picojson_lib)
    target_compile_definitions(mxl PRIVATE SPDLOG_FMT_EXTERNAL)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(mxl PRIVATE NDEBUG)
    endif()
    add_library(mxl::mxl ALIAS mxl)
endif()

set(PLAYER_SOURCES
    src/format.cpp
    src/uuid_util.cpp
    src/v210.cpp
    src/timecode.cpp
    src/anc.cpp
    src/audio.cpp
    src/pattern.cpp
    src/motion.cpp
    src/playlist.cpp
    src/placeholders.cpp
    src/config.cpp
    src/ids.cpp
    src/text.cpp
    src/overlay.cpp
    src/process.cpp
    src/conform.cpp
    src/decode.cpp
    src/media.cpp
    src/mxl_io.cpp
    src/http_server.cpp
    src/nmos.cpp
    src/output.cpp
    src/app.cpp
)

add_library(player_core STATIC ${PLAYER_SOURCES})
target_include_directories(player_core PUBLIC src ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(player_core
    PUBLIC
        PkgConfig::AV
        PkgConfig::FT
        nlohmann_json::nlohmann_json
        Threads::Threads
        mxl::mxl
)
target_compile_options(player_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(mxl-test-player src/main.cpp)
target_link_libraries(mxl-test-player PRIVATE player_core)
target_compile_options(mxl-test-player PRIVATE -Wall -Wextra)

if(PLAYER_BUILD_TESTS)
    add_executable(player_tests tests/test_player.cpp)
    target_include_directories(player_tests PRIVATE third_party/doctest)
    target_link_libraries(player_tests PRIVATE player_core)
    target_compile_definitions(player_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
endif()

install(TARGETS mxl-test-player RUNTIME DESTINATION bin)
install(DIRECTORY assets/fonts DESTINATION share/mxl-test-player)
install(DIRECTORY web/dist DESTINATION share/mxl-test-player/web OPTIONAL)
