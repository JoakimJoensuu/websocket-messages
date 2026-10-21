cmake_minimum_required(VERSION 3.21)
project(websocket-framer LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  file(REMOVE "${CMAKE_SOURCE_DIR}/compile_commands.json")
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC COPY_ON_ERROR)
endif()

add_library(wsf_warnings INTERFACE)
target_compile_options(
  wsf_warnings
  INTERFACE -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wformat=2
            -Wmissing-prototypes
            -Wshadow
            -Wstrict-prototypes)
add_library(wsf_sanitize INTERFACE)
if(PROJECT_IS_TOP_LEVEL AND (CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "GNU"))
  target_compile_options(wsf_sanitize INTERFACE -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(wsf_sanitize INTERFACE -fsanitize=address,undefined)
endif()

option(WSF_HOSTED "Heap-backed decoder and encode" ON)

include(FetchContent)
set(WSC_HOSTED ${WSF_HOSTED} CACHE BOOL "Heap-backed decoder and wsc_encode" FORCE)
set(WSC_TESTS OFF CACHE BOOL "" FORCE)
set(WSC_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  websocket-codec
  GIT_REPOSITORY https://github.com/JoakimJoensuu/websocket-codec.git
  GIT_TAG 4eea5623edc2fce06062188aa6891ec1e7097ed2)
FetchContent_MakeAvailable(websocket-codec)
target_link_libraries(wsc PRIVATE wsf_sanitize)

add_library(wsf src/wsf.c)
if(WSF_HOSTED)
  target_compile_definitions(wsf PUBLIC WSF_HOSTED=1)
else()
  target_compile_options(wsf PRIVATE -ffreestanding)
endif()

target_include_directories(wsf PUBLIC include)
target_compile_features(wsf PUBLIC c_std_23)
target_link_libraries(wsf PRIVATE wsc wsf_warnings wsf_sanitize)

option(WSF_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(WSF_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

if(WSF_EXAMPLES AND WSF_HOSTED)
  add_subdirectory(examples)
endif()

if(WSF_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
