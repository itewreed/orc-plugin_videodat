if(DEFINED ORC_INTREE_SDK_DIR AND TARGET orc-plugin-sdk)
    set(VIDEODAT_TEST_SDK_TARGET orc-plugin-sdk)
elseif(TARGET orc::plugin-sdk)
    set(VIDEODAT_TEST_SDK_TARGET orc::plugin-sdk)
elseif(TARGET orc-plugin-sdk)
    set(VIDEODAT_TEST_SDK_TARGET orc-plugin-sdk)
else()
    message(FATAL_ERROR "Neither orc::plugin-sdk nor orc-plugin-sdk is available for tests")
endif()

function(videodat_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${VIDEODAT_TEST_SDK_TARGET})
    if(UNIX AND NOT APPLE)
        target_link_libraries(${name} PRIVATE dl)
    endif()
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(NAME ${name} COMMAND ${name} ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

set(_stage_sources
    ../src/videodat_sink_stage.cpp
    ../src/decoder/vd_util.cpp
    ../src/decoder/vd_json.cpp
    ../src/decoder/vd_prng.cpp
    ../src/decoder/vd_crypto.cpp
    ../src/decoder/vd_packets.cpp
    ../src/decoder/vd_signal.cpp
    ../src/decoder/vd_assemble.cpp
    ../src/decoder/vd_1990.cpp
    ../src/decoder/vd_decoder.cpp
)

videodat_add_test(videodat_stage_smoke_test stage_smoke_test.cpp ${_stage_sources})
videodat_add_test(videodat_trigger_test trigger_test.cpp ${_stage_sources})
videodat_add_test(videodat_plugin_entrypoints_test plugin_entrypoints_test.cpp
    ../src/plugin.cpp ${_stage_sources})
