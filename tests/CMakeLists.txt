# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

foreach(_runtime_variant in-tree installed)
    set(_runtime_staging_dir
        ${CMAKE_CURRENT_BINARY_DIR}/runtime_staging/${_runtime_variant})
    add_test(NAME cmake.runtime_staging.${_runtime_variant}.configure
        COMMAND ${CMAKE_COMMAND}
            -DREXGLUE_HELPERS=${PROJECT_SOURCE_DIR}/cmake/rexglue_helpers.cmake
            -DVARIANT=${_runtime_variant}
            -S ${CMAKE_CURRENT_SOURCE_DIR}/runtime_staging
            -B ${_runtime_staging_dir}
    )
    add_test(NAME cmake.runtime_staging.${_runtime_variant}.build
        COMMAND ${CMAKE_COMMAND} --build ${_runtime_staging_dir}
            --config $<CONFIG> --target runtime_staging_host
    )
    set_tests_properties(cmake.runtime_staging.${_runtime_variant}.build
        PROPERTIES DEPENDS cmake.runtime_staging.${_runtime_variant}.configure)
    unset(_runtime_staging_dir)
endforeach()

# PPC instruction tests (requires PPC toolchain)
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()
