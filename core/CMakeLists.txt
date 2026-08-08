# --- Codegen client stub for gRPC ServerReflection (third_party/grpc_reflection/reflection.proto) ---
# vcpkg does NOT export the reflection client stub -> generate it with protoc + grpc_cpp_plugin.
set(REFL_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/grpc_reflection)
set(REFL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${REFL_GEN_DIR})
add_custom_command(
  OUTPUT ${REFL_GEN_DIR}/reflection.pb.cc ${REFL_GEN_DIR}/reflection.pb.h
         ${REFL_GEN_DIR}/reflection.grpc.pb.cc ${REFL_GEN_DIR}/reflection.grpc.pb.h
  COMMAND protobuf::protoc
  ARGS --proto_path=${REFL_PROTO_DIR}
       --cpp_out=${REFL_GEN_DIR}
       --grpc_out=${REFL_GEN_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       ${REFL_PROTO_DIR}/reflection.proto
  DEPENDS ${REFL_PROTO_DIR}/reflection.proto protobuf::protoc gRPC::grpc_cpp_plugin
  COMMENT "Generating gRPC reflection client stub"
  VERBATIM
)

# core — static lib, pure C++ (cross-platform). The UI links against this. README §3.
add_library(core STATIC
  src/infra/variables/variable_resolver.cpp
  src/infra/serialization/json_codec.cpp
  src/infra/serialization/request_json_mapper.cpp
  src/infra/serialization/payload/http_payload_json.cpp
  src/infra/serialization/payload/grpc_payload_json.cpp
  src/infra/serialization/payload/graphql_payload_json.cpp
  src/infra/serialization/payload/ws_payload_json.cpp
  src/infra/serialization/payload/kafka_payload_json.cpp
  src/infra/serialization/payload/soap_payload_json.cpp
  src/infra/serialization/payload/ldap_payload_json.cpp
  src/infra/serialization/field_json.cpp
  src/infra/serialization/kafka_field_json.cpp
  src/infra/serialization/misc_field_json.cpp
  src/infra/serialization/text_format.cpp
  src/app/send_request_saga.cpp
  src/app/request_orchestrator.cpp
  src/app/composition_root.cpp
  src/app/repo_adapters.cpp
  src/app/aliasify.cpp
  src/infra/transport/http/native_http_sender.cpp
  src/infra/transport/grpc/native_grpc_sender.cpp
  src/infra/transport/grpc/grpc_marshal.cpp
  src/infra/transport/grpc/grpc_streaming.cpp
  src/infra/transport/graphql/native_graphql_sender.cpp
  src/infra/persistence/collection_repository.cpp
  src/infra/variables/domain_variable_resolver.cpp
  src/infra/import/import_service.cpp
  src/infra/platform/fs_util.cpp
  src/infra/platform/mem_probe.cpp
  src/infra/persistence/collection_store.cpp
  src/infra/persistence/store_naming_util.cpp
  src/infra/persistence/collection_store_crud.cpp
  src/infra/persistence/request_naming.cpp
  src/infra/cache/ram_cache_driver.cpp
  src/infra/cache/disk_cache_driver.cpp
  src/infra/cache/response_cache.cpp
  src/infra/cache/cache_codec.cpp
  src/infra/persistence/session_store.cpp
  src/infra/persistence/environment_store.cpp
  src/infra/persistence/env_crypto.cpp
  src/infra/persistence/order_key.cpp
  src/infra/persistence/app_config_store.cpp
  src/infra/transport/grpc/grpc_descriptors.cpp
  src/infra/transport/ws/ws_sender.cpp
  src/infra/transport/ws/ws_session.cpp
  src/infra/transport/ws/ws_pump.cpp
  src/infra/transport/kafka/kafka_sender.cpp
  src/infra/transport/kafka/kafka_conf.cpp
  src/infra/transport/kafka/kafka_producer.cpp
  src/infra/transport/kafka/kafka_consumer.cpp
  src/infra/transport/shared/sse_parser.cpp
  src/infra/auth/oauth2_token_provider.cpp
  src/infra/transport/graphql/gql_introspection.cpp
  src/infra/transport/http/blocking_fetch.cpp
  src/infra/transport/soap/native_soap_sender.cpp
  src/infra/transport/ldap/native_ldap_sender.cpp
  src/infra/transport/ldap/ldap_conn.cpp
  src/infra/transport/ldap/ldap_search.cpp
  src/infra/transport/kafka/avro_serde.cpp
  src/infra/transport/kafka/schema_registry_client.cpp
  src/infra/transport/graphql/gql_operation.cpp
  src/infra/transport/graphql/gql_ws_protocol.cpp
  src/infra/import/shell_tokenize.cpp
  src/infra/import/curl_importer.cpp
  src/infra/export/curl_export.cpp
  src/infra/import/grpc_importer.cpp
  src/infra/import/graphql_importer.cpp
  src/infra/import/ldap_importer.cpp
  ${REFL_GEN_DIR}/reflection.pb.cc
  ${REFL_GEN_DIR}/reflection.grpc.pb.cc
)

# openldap (vcpkg) ships no CMake config -> find its static libs by hand (LDAP sender, SPEC_ldap).
find_library(LDAP_LIB NAMES ldap REQUIRED)
find_library(LBER_LIB NAMES lber REQUIRED)

# include/ = public headers (the UI only sees this); src/ = private.
# generated/ = reflection stub generated at build time (grpc_descriptors.cpp includes "reflection.grpc.pb.h").
target_include_directories(core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${REFL_GEN_DIR}
)

target_link_libraries(core
  PUBLIC
    nlohmann_json::nlohmann_json   # used in codec (internal header); public DTOs do not leak it
  PRIVATE
    cpr::cpr                       # REST (libcurl)
    CURL::libcurl                  # WebSocket (curl_ws_send/recv — SPEC_websocket §3)
    gRPC::grpc++                   # gRPC C++
    protobuf::libprotobuf          # descriptor / dynamic_message / json_util
    protobuf::libprotoc            # compiler::Importer / DiskSourceTree (load .proto at runtime)
    RdKafka::rdkafka++             # Kafka producer+consumer (SPEC_kafka §6) — confined to src/infra/transport/kafka
    avro-cpp::avrocpp              # Kafka Avro value serde (SPEC_kafka Avro v1) — confined to src/infra/transport/kafka
    OpenSSL::Crypto                # env-value encrypt-at-rest (env_crypto.cpp)
    ${LDAP_LIB} ${LBER_LIB}        # LDAP sender (SPEC_ldap) — confined to src/infra/transport/ldap
    OpenSSL::SSL                   # libldap TLS (ldaps) rides libssl
)

target_compile_features(core PUBLIC cxx_std_17)

# exhaustiveness guard: a new RequestType alternative must fail loudly, not fall into a default branch.
# -Wno-missing-field-initializers: Parts{url} + NSDMI partial init is the house idiom, not a bug.
target_compile_options(core PRIVATE -Wall -Wextra -Wswitch -Wno-missing-field-initializers)
