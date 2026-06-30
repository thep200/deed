add_executable(core_tests core_tests/test_main.cpp core_tests/stream_sink_test.cpp
               core_tests/ws_session_test.cpp core_tests/sse_parser_test.cpp
               core_tests/gql_ws_protocol_test.cpp core_tests/mapper_roundtrip_test.cpp
               core_tests/saga_test.cpp core_tests/repository_test.cpp
               core_tests/import_service_test.cpp core_tests/persistence_repo_test.cpp
               core_tests/field_json_test.cpp)
target_link_libraries(core_tests PRIVATE core)
# White-box access to infra mappers/sagas (core/src) for REFACTOR_SPEC P2+ tests.
target_include_directories(core_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
add_test(NAME core_tests COMMAND core_tests)

# REFACTOR_SPEC P1: domain value-object tests. Deliberately does NOT link `core` — the domain layer is
# header-only, pure STL, and must compile/run standalone (architectural guarantee, §2).
add_executable(domain_tests core_tests/domain_tests.cpp)
target_include_directories(domain_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_compile_features(domain_tests PRIVATE cxx_std_17)
add_test(NAME domain_tests COMMAND domain_tests)

# REFACTOR_SPEC §11.5: dependency gate — fail if JSON/transport libs leak into domain or app headers.
add_test(NAME domain_purity
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/scripts/check_domain_purity.sh ${CMAKE_SOURCE_DIR})

# RESTRUCTURE_PLAN §5: architecture gate — fail if legacy parallel dirs re-appear, domain depends on
# app/infra, or transport libs leak outside src/infra/transport. Locks the 3-layer cleanup in place.
add_test(NAME layering_gate
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/scripts/check_layering.sh ${CMAKE_SOURCE_DIR})

# REFACTOR_SPEC P5/P6 e2e: real HTTP + GraphQL + WebSocket through the new domain stack (CoreApiClient),
# verified against the Go test server (tests/testserver, stdlib-only, builds offline). One runner starts the
# server once and drives both binaries (HTTP/GraphQL via http_e2e, WebSocket via ws_e2e). SKIPs if no Go.
add_executable(http_e2e core_tests/http_e2e_test.cpp)
target_link_libraries(http_e2e PRIVATE core)
add_executable(ws_e2e core_tests/ws_e2e_test.cpp)
target_link_libraries(ws_e2e PRIVATE core)
# gRPC e2e talks to the reflection echo server (tests/grpcserver, needs grpc-go in the module cache; the
# runner SKIPs it if that server can't build offline).
add_executable(grpc_e2e core_tests/grpc_e2e_test.cpp)
target_link_libraries(grpc_e2e PRIVATE core)
add_test(NAME e2e
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/tests/run_e2e.sh
                 $<TARGET_FILE:http_e2e>:http $<TARGET_FILE:ws_e2e>:ws $<TARGET_FILE:grpc_e2e>:grpc)
