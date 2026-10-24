add_executable(example hello.c)
target_link_libraries(example PRIVATE wsm wsm_warnings wsm_sanitize)
