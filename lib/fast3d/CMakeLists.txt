# Embedded Fast3D core (docs/fast3d_port_plan.md). Adapted from Emill/n64-fast3d-engine, see LICENSE.txt in this folder.
#
# Standalone:   cmake -S lib/fast3d -B tmp/fast3d && cmake --build tmp/fast3d && ./tmp/fast3d/fast3d_core_test
# From the main project: configure with -DRUSH2_FAST3D=ON. Nothing in the game links this yet; the RT64 renderer is
# still the one that runs.
cmake_minimum_required(VERSION 3.20)
if (NOT CMAKE_PROJECT_NAME)
    project(Fast3DCore LANGUAGES C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
endif()

add_library(fast3d_core STATIC gfx_pc.c gfx_cc.c)
target_include_directories(fast3d_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (NOT MSVC)
    target_link_libraries(fast3d_core PUBLIC m)
endif()

# Offline test: a fake RDRAM and a null rendering backend, no GPU or ROM needed.
add_executable(fast3d_core_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fast3d/test_core.cpp)
target_link_libraries(fast3d_core_test PRIVATE fast3d_core)

enable_testing()
add_test(NAME fast3d_core COMMAND fast3d_core_test)
