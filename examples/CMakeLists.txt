add_executable(example hello.c)
target_link_libraries(example PRIVATE wsf wsf_warnings wsf_sanitize)
