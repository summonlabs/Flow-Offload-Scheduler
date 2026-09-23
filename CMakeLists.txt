cmake_minimum_required(VERSION 3.20)

project(FlowOffloadScheduler
        VERSION 1.0.0
        DESCRIPTION "Deterministic placement authority for flow-processing execution location"
        HOMEPAGE_URL "https://github.com/summonlabs/Flow-Offload-Scheduler"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(FLOW_OFFLOAD_SCHEDULER_BUILD_TOOLS "Build the fos_ctl and fos_schedd executables" ON)
option(FLOW_OFFLOAD_SCHEDULER_BUILD_TESTS "Build the Flow Offload Scheduler test suite" ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FLOW_OFFLOAD_SCHEDULER_IS_TOP_LEVEL ON)
else()
  set(FLOW_OFFLOAD_SCHEDULER_IS_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# First-party warning policy.
#
# This is an INTERFACE library linked PRIVATE by every first-party target, so
# neither the warning flags nor the compile definitions reach a consumer of the
# exported package.
# ---------------------------------------------------------------------------
# The flags are applied directly to each first-party target rather than through
# an INTERFACE library, so nothing about this repository's build strictness can
# appear in the exported package.
if(MSVC)
  set(FOS_WARNING_OPTIONS /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /sdl)
  set(FOS_WARNING_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  set(FOS_WARNING_OPTIONS -Wall -Wextra -Wpedantic -Werror -Wshadow)
  set(FOS_WARNING_DEFINITIONS "")
endif()

function(fos_apply_project_warnings target)
  target_compile_options(${target} PRIVATE ${FOS_WARNING_OPTIONS})
  if(FOS_WARNING_DEFINITIONS)
    target_compile_definitions(${target} PRIVATE ${FOS_WARNING_DEFINITIONS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(flow_offload_scheduler
  src/canonical.cpp
  src/digest.cpp
  src/engine.cpp
  src/export.cpp
  src/identity.cpp
  src/model.cpp
  src/placement.cpp
  src/protocol.cpp
  src/reason.cpp
  src/serialize.cpp
  src/service.cpp
  src/socket.cpp
  src/store.cpp
  src/thread_pool.cpp
  src/version.cpp)
add_library(Summon::FlowOffloadScheduler ALIAS flow_offload_scheduler)

set_target_properties(flow_offload_scheduler PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME FlowOffloadScheduler)

target_include_directories(flow_offload_scheduler PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(flow_offload_scheduler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src)

# cxx_std_20 is a usage requirement: a consumer must compile against C++20.
target_compile_features(flow_offload_scheduler PUBLIC cxx_std_20)
fos_apply_project_warnings(flow_offload_scheduler)

if(WIN32)
  # Winsock is required by the transport implementation only. CMake records it
  # as a link-only requirement for the static archive; no header or warning flag
  # from this project reaches a consumer.
  target_link_libraries(flow_offload_scheduler PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(FLOW_OFFLOAD_SCHEDULER_BUILD_TOOLS)
  add_executable(fos_ctl tools/fos_ctl.cpp)
  set_target_properties(fos_ctl PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_link_libraries(fos_ctl PRIVATE flow_offload_scheduler)
  fos_apply_project_warnings(fos_ctl)

  add_executable(fos_schedd tools/fos_schedd.cpp)
  set_target_properties(fos_schedd PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_link_libraries(fos_schedd PRIVATE flow_offload_scheduler)
  fos_apply_project_warnings(fos_schedd)

  add_executable(fos_bench tools/fos_bench.cpp)
  set_target_properties(fos_bench PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_link_libraries(fos_bench PRIVATE flow_offload_scheduler)
  fos_apply_project_warnings(fos_bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING AND FLOW_OFFLOAD_SCHEDULER_BUILD_TESTS)
  add_library(fos_test_support STATIC
    tests/harness.cpp
    tests/support/process.cpp)
  set_target_properties(fos_test_support PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_include_directories(fos_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(fos_test_support PRIVATE flow_offload_scheduler)
  fos_apply_project_warnings(fos_test_support)

  set(FOS_TEST_TARGETS
    core
    placement
    migration
    persistence
    property
    concurrency
    transport)

  foreach(test_name IN LISTS FOS_TEST_TARGETS)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    set_target_properties(test_${test_name} PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_link_libraries(test_${test_name} PRIVATE
      flow_offload_scheduler fos_test_support)
    fos_apply_project_warnings(test_${test_name})
    add_test(NAME ${test_name} COMMAND test_${test_name})
  endforeach()

  add_executable(test_process tests/test_process.cpp)
  set_target_properties(test_process PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_link_libraries(test_process PRIVATE
    flow_offload_scheduler fos_test_support)
  fos_apply_project_warnings(test_process)
  add_test(NAME process COMMAND test_process $<TARGET_FILE:fos_schedd>)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(TARGETS flow_offload_scheduler
        EXPORT FlowOffloadSchedulerTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FLOW_OFFLOAD_SCHEDULER_BUILD_TOOLS)
  install(TARGETS fos_ctl fos_schedd fos_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(FOS_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/FlowOffloadScheduler)

install(EXPORT FlowOffloadSchedulerTargets
        FILE FlowOffloadSchedulerTargets.cmake
        NAMESPACE Summon::
        DESTINATION ${FOS_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlowOffloadSchedulerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FlowOffloadSchedulerConfig.cmake
  INSTALL_DESTINATION ${FOS_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FlowOffloadSchedulerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FlowOffloadSchedulerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FlowOffloadSchedulerConfigVersion.cmake
        DESTINATION ${FOS_CMAKE_INSTALL_DIR})

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/FlowOffloadScheduler)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/FlowOffloadScheduler)
