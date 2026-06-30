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
  src/variables/variable_resolver.cpp
  src/codec/json_codec.cpp
  src/infra/serialization/request_json_mapper.cpp
  src/infra/serialization/field_json.cpp
  src/app/send_request_saga.cpp
  src/app/request_orchestrator.cpp
  src/app/composition_root.cpp
  src/infra/senders/native_http_sender.cpp
  src/infra/senders/native_grpc_sender.cpp
  src/infra/senders/native_graphql_sender.cpp
  src/infra/senders/ws_sender_adapter.cpp
  src/infra/persistence/collection_repository.cpp
  src/infra/variables/domain_variable_resolver.cpp
  src/infra/import_export/import_service.cpp
  src/infra/fs_util.cpp
  src/infra/mem_probe.cpp
  src/persistence/collection_store.cpp
  src/persistence/request_naming.cpp
  src/cache/ram_cache_driver.cpp
  src/cache/disk_cache_driver.cpp
  src/cache/response_cache.cpp
  src/cache/cache_codec.cpp
  src/persistence/session_store.cpp
  src/persistence/environment_store.cpp
  src/persistence/app_config_store.cpp
  src/sending/grpc_descriptors.cpp
  src/sending/ws_sender.cpp
  src/streaming/sse_parser.cpp
  src/graphql/gql_operation.cpp
  src/graphql/gql_ws_protocol.cpp
  src/import_export/shell_tokenize.cpp
  src/import_export/curl_importer.cpp
  src/import_export/curl_export.cpp
  src/import_export/grpc_importer.cpp
  src/import_export/graphql_importer.cpp
  ${REFL_GEN_DIR}/reflection.pb.cc
  ${REFL_GEN_DIR}/reflection.grpc.pb.cc
)

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
)

target_compile_features(core PUBLIC cxx_std_17)
