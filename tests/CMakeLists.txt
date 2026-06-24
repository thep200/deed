add_executable(core_tests core_tests/test_main.cpp core_tests/stream_sink_test.cpp
               core_tests/ws_session_test.cpp core_tests/sse_parser_test.cpp
               core_tests/gql_ws_protocol_test.cpp)
target_link_libraries(core_tests PRIVATE core)
add_test(NAME core_tests COMMAND core_tests)
