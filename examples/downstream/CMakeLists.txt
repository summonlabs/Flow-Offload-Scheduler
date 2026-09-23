cmake_minimum_required(VERSION 3.20)

# Independent downstream project. It consumes the installed package only: no
# source is shared with the Flow Offload Scheduler repository, and no include
# directory from that tree is referenced.
project(FlowOffloadSchedulerConsumer
        VERSION 1.0.0
        DESCRIPTION "Downstream consumer of the installed Flow Offload Scheduler package"
        LANGUAGES CXX)

find_package(FlowOffloadScheduler 1.0 REQUIRED CONFIG)

add_executable(consumer main.cpp)
set_target_properties(consumer PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_link_libraries(consumer PRIVATE Summon::FlowOffloadScheduler)

# Deliberately looser than the library's own warning policy: if the project's
# /W4 /WX (or -Werror) leaked through the exported target, this build would
# fail here rather than in the library's tree.
if(MSVC)
  target_compile_options(consumer PRIVATE /W2)
else()
  target_compile_options(consumer PRIVATE -w)
endif()

enable_testing()
add_test(NAME consumer COMMAND consumer)
