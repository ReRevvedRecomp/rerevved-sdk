# Checks rex_compute_version and the git query in rex_resolve_version.
# Usage: cmake -DREX_VERSION=<rex_version.cmake> -DSOURCE_DIR=<repo> -P version_test.cmake

include("${REX_VERSION}")

function(expect_version expected)
    rex_compute_version(actual ${ARGN})
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "expected '${expected}', got '${actual}' for: ${ARGN}")
    endif()
endfunction()

set(full_sha "b726fe7dd6c68f4d1f3d917598b1e2d32fc4cdae")

expect_version("0.13.0"
    FLOOR_MAJOR 0 FLOOR_MINOR 13
    GIT_DESCRIBE_EXACT "v0.13.0" GIT_DESCRIBE_LONG "v0.13.0-0-g${full_sha}")
expect_version("0.13.0.5-dev.gb726fe7d"
    FLOOR_MAJOR 0 FLOOR_MINOR 13
    GIT_DESCRIBE_LONG "v0.13.0-5-g${full_sha}" BRANCH_NAME "main")
expect_version("0.13.0.5-rc.gb726fe7d"
    FLOOR_MAJOR 0 FLOOR_MINOR 13
    GIT_DESCRIBE_LONG "v0.13.0-5-g${full_sha}" BRANCH_NAME "release/0.13.1")
expect_version("0.14.0-dev.gb726fe7d"
    FLOOR_MAJOR 0 FLOOR_MINOR 14
    GIT_DESCRIBE_LONG "v0.13.0-5-g${full_sha}")

# The resolved hash must not depend on git's object-count-based abbreviation.
rex_resolve_version(resolved FLOOR_MAJOR 9999 FLOOR_MINOR 0 SOURCE_DIR "${SOURCE_DIR}")
if(resolved MATCHES "-(dev|rc)\\.g([0-9a-f]+)$")
    set(resolved_sha "${CMAKE_MATCH_2}")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    string(SUBSTRING "${head}" 0 8 expected_sha)
    if(NOT resolved_sha STREQUAL expected_sha)
        message(FATAL_ERROR "resolved '${resolved}' does not end in g${expected_sha}")
    endif()
endif()
