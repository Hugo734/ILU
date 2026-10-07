add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE core)
target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME protocol COMMAND test_protocol)
