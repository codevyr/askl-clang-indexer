cmake_minimum_required(VERSION 3.16)
project(askl-clang-indexer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# System dependencies
find_package(Protobuf CONFIG REQUIRED)
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
  GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(CLI11)

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

add_executable(askl-clang-indexer
  src/main.cpp
  src/compilation_db.cpp
  src/indexer.cpp
  src/stage1.cpp
  src/stage2.cpp
  src/symbol_table.cpp
  src/proto_builder.cpp
  ${PROTO_SRCS}
)

target_include_directories(askl-clang-indexer PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LIBCLANG_INCLUDE_DIRS}
)

target_link_libraries(askl-clang-indexer PRIVATE
  ${LIBCLANG_LIB}
  protobuf::libprotobuf
  Threads::Threads
  CLI11::CLI11
)
