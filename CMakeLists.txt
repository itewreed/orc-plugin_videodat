cmake_minimum_required(VERSION 3.20)

project(orc-plugin-videodat
    VERSION 0.1.0
    DESCRIPTION "Decode-Orc VideoDat decoder sink stage plugin"
    LANGUAGES CXX
)

set(ORC_PLUGIN_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Plugin version string embedded in StagePluginDescriptor")

# On tagged CI builds, derive plugin version from tag names like v1.2.3
# unless the caller explicitly overrides ORC_PLUGIN_VERSION.
if(ORC_PLUGIN_VERSION STREQUAL PROJECT_VERSION)
    if(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND DEFINED ENV{GITHUB_REF_NAME})
        set(_orc_ref_name "$ENV{GITHUB_REF_NAME}")
        if(_orc_ref_name MATCHES "^v(.+)$")
            set(ORC_PLUGIN_VERSION "${CMAKE_MATCH_1}")
            set(ORC_PLUGIN_VERSION "${ORC_PLUGIN_VERSION}" CACHE STRING
                "Plugin version string embedded in StagePluginDescriptor" FORCE)
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build plugin tests" ON)

# The SDK's stage_instructions.h includes <windows.h>, whose min/max macros
# break std::numeric_limits<>::max() in later SDK headers.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# channel-videodat-decoder checkout whose Python scripts are copied next to
# the plugin (videodat-decoder/), where the stage looks for them by default.
set(VIDEODAT_DECODER_DIR "" CACHE PATH
    "channel-videodat-decoder checkout to bundle with the plugin (optional)")

if(DEFINED ORC_INTREE_SDK_DIR)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DecodeOrcPluginSDKHelpers.cmake")

    # Since Decode-Orc 2.0 the SDK include surface is fully self-contained
    # under orc/sdk/include (<orc/plugin/...> and <orc/stage/...>); private
    # host include trees must not be exposed to plugin code.
    add_library(orc-sdk-includes INTERFACE)

    # Try to find fmt via CMake config (works with vcpkg toolchain on Windows).
    # Fall back to a Nix store glob for Linux/macOS Nix dev shells.
    find_package(fmt CONFIG QUIET)
    if(fmt_FOUND)
        target_link_libraries(orc-sdk-includes INTERFACE fmt::fmt-header-only)
    else()
        file(GLOB _orc_fmt_include_candidates "/nix/store/*-fmt-*/include")
        list(LENGTH _orc_fmt_include_candidates _orc_fmt_include_count)
        if(_orc_fmt_include_count GREATER 0)
            list(GET _orc_fmt_include_candidates 0 _orc_fmt_include_dir)
            target_include_directories(orc-sdk-includes INTERFACE ${_orc_fmt_include_dir})
        endif()
    endif()

    add_library(orc-plugin-sdk INTERFACE)
    target_include_directories(orc-plugin-sdk INTERFACE
        ${ORC_INTREE_SDK_DIR}/orc/sdk/include
    )
    target_link_libraries(orc-plugin-sdk INTERFACE orc-sdk-includes)
else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DecodeOrcPluginSDKHelpers.cmake")
    find_package(decode-orc-plugin-sdk REQUIRED)
endif()

set(VIDEODAT_STAGE_NAME "videodat_decoder")
set(VIDEODAT_PLUGIN_TARGET "orc-stage-plugin-videodat")

set(VIDEODAT_STAGE_SOURCES
    src/videodat_sink_stage.cpp
    src/vbi_tbc_writer.cpp
    src/decoder_process.cpp
)

orc_add_stage_plugin(
    ${VIDEODAT_PLUGIN_TARGET}
    OUTPUT_NAME ${VIDEODAT_PLUGIN_TARGET}
    PLUGIN_VERSION ${ORC_PLUGIN_VERSION}
    SOURCES
        src/plugin.cpp
        ${VIDEODAT_STAGE_SOURCES}
)

target_include_directories(${VIDEODAT_PLUGIN_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(VIDEODAT_DECODER_DIR)
    file(GLOB _videodat_scripts "${VIDEODAT_DECODER_DIR}/*.py")
    if(NOT _videodat_scripts)
        message(FATAL_ERROR "VIDEODAT_DECODER_DIR has no Python scripts: ${VIDEODAT_DECODER_DIR}")
    endif()
    foreach(_script IN LISTS _videodat_scripts)
        get_filename_component(_name "${_script}" NAME)
        add_custom_command(TARGET ${VIDEODAT_PLUGIN_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${VIDEODAT_PLUGIN_TARGET}>/videodat-decoder"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_script}"
                "$<TARGET_FILE_DIR:${VIDEODAT_PLUGIN_TARGET}>/videodat-decoder/${_name}")
    endforeach()
    install(FILES ${_videodat_scripts}
        DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}/videodat-decoder")
endif()

# Build-info emitter for the release manifest (host ABI 12 and later).
# Compiled with the same toolchain and SDK headers as the plugin, it prints
# the descriptor's ABI version and toolchain tag so scripts/package_local.sh
# can declare them in orc-plugin-manifest.yaml without guessing.
if(TARGET orc::plugin-sdk)
    set(_orc_build_info_sdk_target orc::plugin-sdk)
else()
    set(_orc_build_info_sdk_target orc-plugin-sdk)
endif()

add_executable(orc-plugin-build-info tools/plugin_build_info.cpp)
target_link_libraries(orc-plugin-build-info PRIVATE ${_orc_build_info_sdk_target})
target_include_directories(orc-plugin-build-info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(orc-plugin-build-info PRIVATE
    ORC_STAGE_PLUGIN_VERSION="${ORC_PLUGIN_VERSION}")
set_target_properties(orc-plugin-build-info PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
