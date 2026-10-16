# Lane Wars engine — C++ implementation (native build: tests, headless matches, shared library).
# The WebAssembly build is done by ../build-cpp.sh (Emscripten or wasi-sdk), not by CMake.
#
#   cmake -S cpp -B cpp/build && cmake --build cpp/build -j && ctest --test-dir cpp/build
cmake_minimum_required(VERSION 3.16)
project(moba_engine_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The simulation must be bit-identical to the Rust engine (and between machines), so floating point
# must follow IEEE single precision exactly: never fuse a*b+c into one FMA, never reassociate.
if(MSVC)
  add_compile_options(/fp:precise /W3)
else()
  add_compile_options(-ffp-contract=off -fno-fast-math -fno-exceptions -Wall -Wextra -Wno-unused-parameter)
endif()

# CI builds with -DMOBA_WERROR=ON so new warnings fail the build; off by default for other compilers.
option(MOBA_WERROR "Treat compiler warnings as errors" OFF)
if(MOBA_WERROR)
  add_compile_options($<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Everything except the C ABI, for the tests and examples.
add_library(moba_core STATIC ${ENGINE_SOURCES})
target_include_directories(moba_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The C ABI as a shared library: libmoba_engine_cpp.so / .dylib / moba_engine_cpp.dll.
# Same exported functions as the Rust engine (and as the .wasm), so tools/ can drive either one.
add_library(moba_engine_cpp SHARED ${ENGINE_SOURCES})
target_include_directories(moba_engine_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(moba_engine_cpp PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE moba_core)
target_compile_definitions(engine_tests PRIVATE MOBA_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(headless examples/headless.cpp)
target_link_libraries(headless PRIVATE moba_core)

enable_testing()
add_test(NAME engine_tests COMMAND engine_tests)
