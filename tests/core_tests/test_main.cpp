// Unit tests for Core — public API only (include/core/). No UI needed.
// Minimal harness: each *_test.cpp owns its checks; main() folds the fail counts (CTest reads exit code).
#include <cstdio>

int run_variables_export_tests();
int run_cache_config_tests();
int run_persistence_store_tests();
int run_engine_import_tests();
int run_stream_sink_tests();
int run_ws_session_tests();
int run_sse_parser_tests();
int run_gql_ws_protocol_tests();
int run_mapper_roundtrip_tests();
int run_saga_tests();
int run_saga_protocol_tests();
int run_saga_cancel_tests();
int run_repository_tests();
int run_import_service_tests();
int run_persistence_repo_tests();
int run_field_json_tests();
int run_gql_introspection_tests();
int run_oauth2_provider_tests();
int run_avro_serde_tests();
int run_soap_tests();
int run_ldap_tests();
int run_order_key_tests();

int main() {
    int failed = 0;
    failed += run_variables_export_tests();
    failed += run_cache_config_tests();
    failed += run_persistence_store_tests();
    failed += run_engine_import_tests();

    failed += run_stream_sink_tests();
    failed += run_ws_session_tests();
    failed += run_sse_parser_tests();
    failed += run_gql_ws_protocol_tests();
    failed += run_mapper_roundtrip_tests();
    failed += run_saga_tests();
    failed += run_saga_protocol_tests();
    failed += run_saga_cancel_tests();
    failed += run_repository_tests();
    failed += run_import_service_tests();
    failed += run_persistence_repo_tests();
    failed += run_field_json_tests();
    failed += run_gql_introspection_tests();
    failed += run_oauth2_provider_tests();
    failed += run_avro_serde_tests();
    failed += run_soap_tests();
    failed += run_order_key_tests();
    failed += run_ldap_tests();

    std::printf("\n==== core_tests: %d failed ====\n", failed);
    return failed == 0 ? 0 : 1;
}
