# Unit Tests for ReXGlue core components

add_executable(unit_tests
    test_memory.cpp
    memory/heap_allocation_test.cpp
    kernel/object_table_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/filesystem_test.cpp
    core/hash_test.cpp
    core/sha256_test.cpp
    core/stream_test.cpp
    core/chrono_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    system/guest_path_test.cpp
    system/achievement_manager_test.cpp
    system/function_dispatcher_test.cpp
    system/mod_dependency_test.cpp
    graphics/diagnostic_util_test.cpp
    input/input_assignment_test.cpp
    codegen/codegen_writer_test.cpp
    codegen/output_partition_test.cpp
    codegen/output_stamp_test.cpp
)

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/diagnostic_util.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    Catch2::Catch2WithMain
)

add_executable(mod_plugin_tests
    system/mod_plugin_test.cpp
)
target_include_directories(mod_plugin_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(mod_plugin_tests PRIVATE
    rexruntime
    Catch2::Catch2WithMain
)

set(REX_TEST_MODS_ROOT "${CMAKE_CURRENT_BINARY_DIR}/mods")
if(REX_PLATFORM STREQUAL "win-amd64")
    set(REX_TEST_MOD_PLATFORM "windows-x64")
elseif(REX_PLATFORM STREQUAL "win-arm64")
    set(REX_TEST_MOD_PLATFORM "windows-arm64")
elseif(REX_PLATFORM STREQUAL "linux-amd64")
    set(REX_TEST_MOD_PLATFORM "linux-x64")
elseif(REX_PLATFORM STREQUAL "linux-arm64")
    set(REX_TEST_MOD_PLATFORM "linux-arm64")
elseif(REX_PLATFORM STREQUAL "mac-amd64")
    set(REX_TEST_MOD_PLATFORM "macos-x64")
elseif(REX_PLATFORM STREQUAL "mac-arm64")
    set(REX_TEST_MOD_PLATFORM "macos-arm64")
else()
    message(FATAL_ERROR "No mod package platform mapping for ${REX_PLATFORM}")
endif()
add_library(test_mod_plugin SHARED system/test_mod_plugin.cpp)
add_library(test_mod_bad_abi SHARED system/test_mod_bad_abi.cpp)
foreach(target test_mod_plugin test_mod_bad_abi)
    target_link_libraries(${target} PRIVATE rexruntime)
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${REX_TEST_MODS_ROOT}/$<TARGET_PROPERTY:${target},OUTPUT_NAME>/code/${REX_TEST_MOD_PLATFORM}"
        RUNTIME_OUTPUT_DIRECTORY "${REX_TEST_MODS_ROOT}/$<TARGET_PROPERTY:${target},OUTPUT_NAME>/code/${REX_TEST_MOD_PLATFORM}"
        DEBUG_POSTFIX "d"
        RELWITHDEBINFO_POSTFIX "rd"
        RELEASE_POSTFIX ""
    )
endforeach()
set_target_properties(test_mod_plugin PROPERTIES OUTPUT_NAME lifecycle)
set_target_properties(test_mod_bad_abi PROPERTIES OUTPUT_NAME bad_abi)
target_compile_definitions(mod_plugin_tests PRIVATE
    REX_TEST_MOD_PLUGIN_PATH="$<TARGET_FILE:test_mod_plugin>"
    REX_TEST_BAD_MOD_PLUGIN_PATH="$<TARGET_FILE:test_mod_bad_abi>"
)
add_dependencies(mod_plugin_tests test_mod_plugin test_mod_bad_abi)

# ReXApp is shipped as source for recomp projects, rather than compiled into
# rexui. Keep its SDK-consumer compilation path covered by the test build.
add_library(rexapp_compile_test OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/rex_app.cpp
)
target_link_libraries(rexapp_compile_test PRIVATE
    rexruntime
    rexui
    rexinput
    rexaudio
)
add_dependencies(unit_tests rexapp_compile_test)

# Strip Tracy/profiling instrumentation from test targets
foreach(target unit_tests mod_plugin_tests)
    target_compile_options(${target} PRIVATE
        -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
        -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
    )
endforeach()

set_target_properties(unit_tests mod_plugin_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests mod_plugin_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# The full historical suite currently includes host-specific cases. Unix
# artifact builds exercise the portable mod loader executable instead.
if(NOT REX_PLATFORM MATCHES "^win-")
    set_target_properties(unit_tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
catch_discover_tests(mod_plugin_tests
    TEST_PREFIX "mod_plugin."
    PROPERTIES
        LABELS "mod_plugin"
        TIMEOUT 60
)
