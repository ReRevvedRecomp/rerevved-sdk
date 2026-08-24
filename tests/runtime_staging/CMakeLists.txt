cmake_minimum_required(VERSION 3.25)
project(runtime_staging_test LANGUAGES CXX)

if(NOT VARIANT MATCHES "^(in-tree|installed)$")
    message(FATAL_ERROR "Set VARIANT to in-tree or installed")
endif()

set(REXGLUE_SHARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/share")
file(MAKE_DIRECTORY "${REXGLUE_SHARE_DIR}")
file(WRITE "${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp" "")
file(WRITE "${REXGLUE_SHARE_DIR}/rex_app.cpp" "")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/librexruntime.so" "runtime")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libTracyClient.so" "tracy")

if(VARIANT STREQUAL "in-tree")
    add_library(rexruntime SHARED IMPORTED)
    set_target_properties(rexruntime PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/librexruntime.so")
    add_library(rex::runtime ALIAS rexruntime)
    add_library(TracyClient SHARED IMPORTED)
    set_target_properties(TracyClient PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libTracyClient.so")
else()
    add_library(rex::runtime SHARED IMPORTED)
    set_target_properties(rex::runtime PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/librexruntime.so")
    add_library(rex::TracyClient SHARED IMPORTED)
    set_target_properties(rex::TracyClient PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libTracyClient.so")
endif()

# Exercise the Unix branch while keeping this regression fixture runnable in
# the Windows SDK suite.
set(UNIX TRUE)
set(APPLE FALSE)
set(WIN32 FALSE)
set(CMAKE_SYSTEM_PROCESSOR runtime-staging-test)
include("${REXGLUE_HELPERS}")

add_executable(runtime_staging_host "${CMAKE_CURRENT_BINARY_DIR}/main.cpp")
set_target_properties(runtime_staging_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host")
rexglue_configure_target(runtime_staging_host)
add_custom_command(TARGET runtime_staging_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E compare_files
        "${CMAKE_CURRENT_BINARY_DIR}/librexruntime.so"
        "$<TARGET_FILE_DIR:runtime_staging_host>/librexruntime.so"
    COMMAND ${CMAKE_COMMAND} -E compare_files
        "${CMAKE_CURRENT_BINARY_DIR}/libTracyClient.so"
        "$<TARGET_FILE_DIR:runtime_staging_host>/libTracyClient.so"
    VERBATIM
)
