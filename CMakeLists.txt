cmake_minimum_required(VERSION 3.20)
project(Puzzle71Solver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PUZZLE71_BUILD_CUDA "Build the CUDA-backed GpuEngine" OFF)

# All targets (puzzle71_tests, puzzle71_solver, puzzle71_gui, and when
# PUZZLE71_BUILD_CUDA=ON, puzzle71_engine_cuda) are defined in tests/CMakeLists.txt.
add_subdirectory(tests)
