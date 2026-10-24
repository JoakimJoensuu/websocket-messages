cmake_minimum_required(VERSION 3.21)
project(websocket-messages LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  file(REMOVE "${CMAKE_SOURCE_DIR}/compile_commands.json")
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC COPY_ON_ERROR)
endif()

add_library(wsm_warnings INTERFACE)
target_compile_options(
  wsm_warnings
  INTERFACE -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wformat=2
            -Wmissing-prototypes
            -Wshadow
            -Wstrict-prototypes)
add_library(wsm_sanitize INTERFACE)
if(PROJECT_IS_TOP_LEVEL AND (CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "GNU"))
  target_compile_options(wsm_sanitize INTERFACE -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(wsm_sanitize INTERFACE -fsanitize=address,undefined)
endif()

option(WSM_HOSTED "Heap-backed decoder and encode" ON)

include(FetchContent)
set(WSC_HOSTED ${WSM_HOSTED} CACHE BOOL "Heap-backed decoder and wsc_encode" FORCE)
set(WSC_TESTS OFF CACHE BOOL "" FORCE)
set(WSC_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  websocket-codec
  GIT_REPOSITORY https://github.com/JoakimJoensuu/websocket-codec.git
  GIT_TAG 4ffcb3ec67dea1edeaeff87baaea3abe5eca58a2)
FetchContent_MakeAvailable(websocket-codec)
target_link_libraries(wsc PRIVATE wsm_sanitize)

add_library(wsm src/wsm.c)
if(WSM_HOSTED)
  target_compile_definitions(wsm PUBLIC WSM_HOSTED=1)
else()
  target_compile_options(wsm PRIVATE -ffreestanding)
endif()

target_include_directories(wsm PUBLIC include)
target_compile_features(wsm PUBLIC c_std_23)
target_link_libraries(wsm PRIVATE wsc wsm_warnings wsm_sanitize)

option(WSM_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(WSM_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

if(WSM_EXAMPLES AND WSM_HOSTED)
  add_subdirectory(examples)
endif()

if(WSM_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
