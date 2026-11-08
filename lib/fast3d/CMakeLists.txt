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

add_library(fast3d_core STATIC gfx_pc.c gfx_cc.c gfx_opengl.c)
target_include_directories(fast3d_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (NOT MSVC)
    target_link_libraries(fast3d_core PUBLIC m)
endif()

# Offline tests: a fake RDRAM, a null rendering backend and a fake GL. No GPU or ROM needed.
set(FAST3D_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fast3d)
add_executable(fast3d_core_test ${FAST3D_TESTS}/test_core.cpp)
target_include_directories(fast3d_core_test PRIVATE ${FAST3D_TESTS})
target_link_libraries(fast3d_core_test PRIVATE fast3d_core)

add_executable(fast3d_opengl_test ${FAST3D_TESTS}/test_opengl.cpp)
target_include_directories(fast3d_opengl_test PRIVATE ${FAST3D_TESTS})
target_link_libraries(fast3d_opengl_test PRIVATE fast3d_core)

# Dumps generated GLSL; tests/fast3d/validate_glsl.js compiles it in WebGL2 (needs node and playwright, so it is not a ctest).
add_executable(fast3d_dump_glsl ${FAST3D_TESTS}/dump_glsl.c)
target_link_libraries(fast3d_dump_glsl PRIVATE fast3d_core)

enable_testing()
add_test(NAME fast3d_core COMMAND fast3d_core_test)
add_test(NAME fast3d_opengl COMMAND fast3d_opengl_test)
