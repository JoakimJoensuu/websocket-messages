include(FetchContent)
enable_language(CXX)

set(CGREEN_WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CGREEN_WITH_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(CGREEN_WITH_XML OFF CACHE BOOL "" FORCE)
set(CGREEN_WITH_LIBXML2 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  cgreen
  GIT_REPOSITORY https://github.com/cgreen-devs/cgreen.git
  GIT_TAG 1.7.0
  GIT_SHALLOW TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
FetchContent_MakeAvailable(cgreen)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WSM_HOSTED)
  add_executable(test_wsm_hosted wsm_hosted.c)
  target_link_libraries(test_wsm_hosted PRIVATE wsm wsm_warnings wsm_sanitize cgreen_static)
  target_include_directories(test_wsm_hosted SYSTEM PRIVATE ${cgreen_SOURCE_DIR}/include)
  add_test(NAME wsm_hosted COMMAND test_wsm_hosted)
else()
  add_executable(test_wsm_freestanding wsm_freestanding.c)
  target_link_libraries(test_wsm_freestanding PRIVATE wsm wsm_warnings wsm_sanitize cgreen_static)
  target_include_directories(test_wsm_freestanding SYSTEM PRIVATE ${cgreen_SOURCE_DIR}/include)
  add_test(NAME wsm_freestanding COMMAND test_wsm_freestanding)
endif()
