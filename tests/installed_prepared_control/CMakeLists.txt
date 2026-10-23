cmake_minimum_required(VERSION 3.25)

project(rexglue_installed_prepared_control LANGUAGES CXX)

find_package(rexglue 0.25 CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rexruntime_installed_prepared_control control.cpp)
target_link_libraries(rexruntime_installed_prepared_control PRIVATE rex::runtime)

add_executable(rexruntime_installed_companion_control
               companion_control.cpp companion_control_main.cpp)
target_link_libraries(rexruntime_installed_companion_control PRIVATE rex::runtime)
add_custom_command(TARGET rexruntime_installed_companion_control POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:rex::gpu-xenos>
        $<TARGET_FILE_DIR:rexruntime_installed_companion_control>
    VERBATIM)
if(WIN32)
  add_custom_command(TARGET rexruntime_installed_companion_control POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_RUNTIME_DLLS:rexruntime_installed_companion_control>
          $<TARGET_FILE_DIR:rexruntime_installed_companion_control>
      COMMAND_EXPAND_LISTS VERBATIM)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME rexruntime_installed_prepared_control
           COMMAND rexruntime_installed_prepared_control)
endif()

install(TARGETS rexruntime_installed_prepared_control RUNTIME DESTINATION bin)
