cmake_minimum_required(VERSION 3.20)
project(puzzle71_tests LANGUAGES CXX)

option(PUZZLE71_BUILD_CUDA "Build the CUDA-backed GpuEngine" OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# 1. Google Test (v1.14.0)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 2. nlohmann/json (v3.11.3)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# 3. CPR (C++ Requests v1.10.5)
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/libcpr/cpr.git
  GIT_TAG 1.10.5
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_ENABLE_SSL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

# 4. Dear ImGui (v1.90.1)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.1
)
FetchContent_MakeAvailable(imgui)

# ImGui core + Win32 + DX11 backends (compiled as sources, no library)
set(IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

enable_testing()

# Single source list: every production .cpp needed by the test suite exists,
# so the former PUZZLE71_BUILD_PARTIAL_TESTS partial/full split was removed
# (both branches had become identical). GpuEngine (.cu) will arrive as a
# SEPARATE target gated behind PUZZLE71_BUILD_CUDA so this suite never
# requires a CUDA toolchain.
set(TEST_SOURCES
  test_rangekey.cpp
  test_random_chunk_generator.cpp
  test_json_range_logger.cpp
  test_prefix_filter.cpp
  test_found_match_writer.cpp
  test_puzzle_solver.cpp
  test_cpu_engine.cpp
  test_gpu_engine.cpp
  test_cli.cpp
  test_gui_controller.cpp
  test_target_list_loader.cpp
  test_puzzle_database.cpp
  test_network_api.cpp)
set(PROD_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../GuiController.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Int.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../IntMod.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../IntGroup.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Timer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Random.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../RangeKey.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../RandomChunkGenerator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../JsonRangeLogger.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../PrefixFilter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../TargetListLoader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../PuzzleDatabase.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../NetworkAPI.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../FoundMatchWriter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../PuzzleSolver.cpp
  # CpuEngine + GpuEngine + the VanitySearch secp256k1 CPU path it adapts.
  ${CMAKE_CURRENT_SOURCE_DIR}/../CpuEngine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../GpuEngine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../SECP256K1.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Point.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Base58.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../Bech32.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../hash/sha256.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../hash/ripemd160.cpp
  # SECP256K1.cpp's 4-way GetHash160/GetAddress reference the SSE batch
  # entry points even when only the single-point overloads are called.
  ${CMAKE_CURRENT_SOURCE_DIR}/../hash/sha256_sse.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../hash/ripemd160_sse.cpp)

add_library(puzzle71_core STATIC
  ${PROD_SOURCES}
)

target_include_directories(puzzle71_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(puzzle71_core PUBLIC
  nlohmann_json::nlohmann_json
  cpr::cpr
)

if(MSVC)
  target_compile_options(puzzle71_core PRIVATE /W4 /utf-8)
  # NOTE: Int.h:214 uses #ifndef WIN64 (not _WIN64). Without WIN64 defined, MSVC
  # attempts to compile GCC __asm__ / __builtin_ia32_addcarryx_u64 and fails.
  target_compile_definitions(puzzle71_core PUBLIC WIN64 _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(puzzle71_core PRIVATE -Wall -Wextra)
endif()

add_executable(puzzle71_tests
  ${TEST_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)

target_link_libraries(puzzle71_tests PRIVATE
  puzzle71_core
  GTest::gtest_main
)

if(MSVC)
  target_compile_options(puzzle71_tests PRIVATE /W4 /utf-8)
  target_compile_definitions(puzzle71_tests PRIVATE PUZZLE71_NO_MAIN)
else()
  target_compile_options(puzzle71_tests PRIVATE -Wall -Wextra)
  target_compile_definitions(puzzle71_tests PRIVATE PUZZLE71_NO_MAIN)
endif()

add_executable(puzzle71_solver
  ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
)
target_link_libraries(puzzle71_solver PRIVATE puzzle71_core)
if(MSVC)
  target_compile_options(puzzle71_solver PRIVATE /W4 /utf-8)
else()
  target_compile_options(puzzle71_solver PRIVATE -Wall -Wextra)
endif()

if(WIN32)
  add_executable(puzzle71_gui
    ${CMAKE_CURRENT_SOURCE_DIR}/../main_gui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main.cpp
    ${IMGUI_SOURCES})
  target_include_directories(puzzle71_gui PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(puzzle71_gui PRIVATE
    puzzle71_core
    d3d11 dxgi)
  target_compile_options(puzzle71_gui PRIVATE /W4 /utf-8)
  target_compile_definitions(puzzle71_gui PRIVATE PUZZLE71_NO_MAIN)
endif()

if(PUZZLE71_BUILD_CUDA)
  # Target RTX 4060 (Ada Lovelace = 8.9) by default if not overridden by user.
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "89")
  endif()

  enable_language(CUDA)
  find_package(CUDAToolkit 12.0 REQUIRED)

  # Aggressive optimizations for the 6-year-old VanitySearch core
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CUDA>:--use_fast_math>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-O3>
    $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo> # for profiling with Nsight
  )

  # Compile GPUEngine.cu into a static lib
  add_library(puzzle71_engine_cuda STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../GPU/GPUEngine.cu
  )
  set_target_properties(puzzle71_engine_cuda PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_SEPARABLE_COMPILATION OFF
    POSITION_INDEPENDENT_CODE ON
  )
  target_compile_definitions(puzzle71_engine_cuda PRIVATE WIN64)
  target_include_directories(puzzle71_engine_cuda PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../GPU
  )
  target_link_libraries(puzzle71_engine_cuda PUBLIC CUDA::cudart)

  # Propagate CUDA definitions and library through puzzle71_core to all targets
  set_target_properties(puzzle71_core PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
  target_compile_definitions(puzzle71_core PUBLIC PUZZLE71_ENABLE_CUDA PUZZLE71_BUILD_CUDA)
  target_link_libraries(puzzle71_core PUBLIC puzzle71_engine_cuda)

  set_target_properties(puzzle71_tests PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
  set_target_properties(puzzle71_solver PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
  if(WIN32)
    set_target_properties(puzzle71_gui PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
  endif()
endif()

include(GoogleTest)
gtest_discover_tests(puzzle71_tests)
