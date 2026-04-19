cmake_minimum_required(VERSION 3.16)
project(askl-clang-indexer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# System dependencies
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

# Find libclang
# Accept LIBCLANG_PATH / LIBCLANG_INCLUDE_PATH as cmake vars or env vars
find_library(LIBCLANG_LIB clang
  HINTS ${LIBCLANG_PATH} ENV LIBCLANG_PATH
  PATHS /usr/lib /usr/lib/llvm-21/lib
)
find_path(LIBCLANG_INCLUDE_DIRS clang-c/Index.h
  HINTS ${LIBCLANG_INCLUDE_PATH} ENV LIBCLANG_INCLUDE_PATH
  PATHS /usr/include /usr/lib/llvm-21/include
)
if(NOT LIBCLANG_LIB)
  message(FATAL_ERROR "Could not find libclang. Set LIBCLANG_PATH env var.")
endif()
if(NOT LIBCLANG_INCLUDE_DIRS)
  message(FATAL_ERROR "Could not find clang-c/Index.h. Set LIBCLANG_INCLUDE_PATH env var.")
endif()
message(STATUS "Found libclang: ${LIBCLANG_LIB}")
message(STATUS "Found clang headers: ${LIBCLANG_INCLUDE_DIRS}")

# Header-only CLI library
include(FetchContent)
FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.6.2
)
FetchContent_MakeAvailable(CLI11)

# Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Generate protobuf C++ files
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

set(PROTO_SRCS ${PROTO_OUT_DIR}/index.pb.cc)
set(PROTO_HDRS ${PROTO_OUT_DIR}/index.pb.h)

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
  COMMAND protobuf::protoc
    --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    -I${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/index.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/index.proto
  COMMENT "Generating protobuf C++ files"
)

# Static library with all source files except main.cpp
add_library(askl-clang-indexer-lib STATIC
  src/compilation_db.cpp
  src/indexer.cpp
  src/stage1.cpp
  src/stage2.cpp
  src/symbol_table.cpp
  src/proto_builder.cpp
  ${PROTO_SRCS}
)

target_include_directories(askl-clang-indexer-lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LIBCLANG_INCLUDE_DIRS}
)

target_link_libraries(askl-clang-indexer-lib PUBLIC
  ${LIBCLANG_LIB}
  protobuf::libprotobuf
  Threads::Threads
)

# Main executable
add_executable(askl-clang-indexer src/main.cpp)

target_link_libraries(askl-clang-indexer PRIVATE
  askl-clang-indexer-lib
  CLI11::CLI11
)

# Test executable
enable_testing()

add_executable(askl-clang-indexer-test test/indexer_test.cpp)

target_compile_definitions(askl-clang-indexer-test PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test"
)

target_link_libraries(askl-clang-indexer-test PRIVATE
  askl-clang-indexer-lib
  GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(askl-clang-indexer-test)
