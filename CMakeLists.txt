#=============================================================================#
# Limit Order Book -- high-performance matching engine
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/benchmark
#=============================================================================#

cmake_minimum_required(VERSION 3.16)

project(limit-order-book
        VERSION 1.0.0
        DESCRIPTION "Price-time priority matching engine with O(1) cancellation"
        LANGUAGES CXX)

#-----------------------------------------------------------------------------#
# Language standard
#-----------------------------------------------------------------------------#

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++20, never -std=gnu++20

# Default to Release. Benchmarking a debug build measures the debug build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type specified; defaulting to Release")
endif()

#-----------------------------------------------------------------------------#
# The engine library
#-----------------------------------------------------------------------------#

add_library(lob STATIC
    src/LimitOrderBook.cpp
    src/MemoryPool.cpp
)

target_include_directories(lob PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(lob PUBLIC cxx_std_20)

#-----------------------------------------------------------------------------#
# Warnings -- treated as a correctness tool, not decoration
#-----------------------------------------------------------------------------#

if(MSVC)
    target_compile_options(lob PRIVATE /W4 /permissive-)
else()
    target_compile_options(lob PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow                  # a shadowed index variable is a silent bug
        -Wconversion              # narrowing between the width-specific types
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align              # flags loads that would straddle cache lines
        -Wunused
        -Woverloaded-virtual
        -Wdouble-promotion        # accidental float -> double on the hot path
    )
endif()

#-----------------------------------------------------------------------------#
# Release tuning
#
# -march=native is opt-in. It produces the fastest binary for THIS cpu and an
# illegal-instruction crash on any other, which is the wrong default for code
# a recruiter will clone and run on unknown hardware.
#-----------------------------------------------------------------------------#

option(LOB_NATIVE_ARCH "Tune for the building machine's CPU (-march=native)" OFF)

if(NOT MSVC)
    target_compile_options(lob PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-fno-omit-frame-pointer>   # keep perf/flamegraphs usable
    )
    if(LOB_NATIVE_ARCH)
        target_compile_options(lob PUBLIC $<$<CONFIG:Release>:-march=native>)
        message(STATUS "Tuning for local CPU: -march=native")
    endif()
endif()

#-----------------------------------------------------------------------------#
# Sanitizer build -- run this before trusting any benchmark
#
#   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON
#
# The engine caches raw iterators and indices into pooled storage, so ASan and
# UBSan are not optional hygiene here; they are how the design gets verified.
#-----------------------------------------------------------------------------#

option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(LOB_SANITIZE AND NOT MSVC)
    target_compile_options(lob PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(lob PUBLIC -fsanitize=address,undefined)
    message(STATUS "Sanitizers enabled (expect a large slowdown; do not benchmark this build)")
endif()

#-----------------------------------------------------------------------------#
# Benchmark and correctness suite
#-----------------------------------------------------------------------------#

add_executable(benchmark tests/benchmark.cpp)
target_link_libraries(benchmark PRIVATE lob)

enable_testing()
add_test(NAME correctness COMMAND benchmark)
