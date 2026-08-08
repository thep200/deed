#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/domain/common/result.hpp"

namespace core::domain {

enum class RawSubtype { Json, Text, Xml };

struct BodyNone {
  bool operator==(const BodyNone &) const { return true; }
};
// Raw{Json} validity is checked at the use-case layer (IJsonValidator), never in the domain.
struct BodyRaw {
  RawSubtype subtype = RawSubtype::Json;
  std::string text; // kept verbatim
  bool operator==(const BodyRaw &o) const { return subtype == o.subtype && text == o.text; }
};
struct FormField {
  std::string key;
  std::string value;
  bool enabled = true;
  bool operator==(const FormField &o) const {
    return key == o.key && value == o.value && enabled == o.enabled;
  }
};
struct BodyFormUrlEncoded {
  std::vector<FormField> fields;
  bool operator==(const BodyFormUrlEncoded &o) const { return fields == o.fields; }
};
enum class PartKind { Text, File };
struct MultipartPart {
  std::string key;
  PartKind kind = PartKind::Text;
  std::string value;    // when kind == Text
  std::string filePath; // when kind == File
  bool enabled = true;
  bool operator==(const MultipartPart &o) const {
    return key == o.key && kind == o.kind && value == o.value && filePath == o.filePath &&
           enabled == o.enabled;
  }
};
struct BodyMultipart {
  std::vector<MultipartPart> parts;
  bool operator==(const BodyMultipart &o) const { return parts == o.parts; }
};
struct BodyBinary {
  std::string filePath;
  bool operator==(const BodyBinary &o) const { return filePath == o.filePath; }
};

class Body {
public:
  using Variant =
      std::variant<BodyNone, BodyRaw, BodyFormUrlEncoded, BodyMultipart, BodyBinary>;

  static Body none() { return Body(BodyNone{}); }
  static Body raw(RawSubtype subtype, std::string text) {
    return Body(BodyRaw{subtype, std::move(text)});
  }
  static Body formUrlEncoded(std::vector<FormField> fields) {
    return Body(BodyFormUrlEncoded{std::move(fields)});
  }
  // Invariant: every File part must carry a filePath.
  static Result<Body> multipart(std::vector<MultipartPart> parts) {
    for (const auto &p : parts)
      if (p.enabled && p.kind == PartKind::File && p.filePath.empty())
        return Result<Body>::fail(
            {ErrorCode::Validation, "multipart file part needs filePath", "body.multipart.filePath"});
    return Result<Body>::ok(Body(BodyMultipart{std::move(parts)}));
  }
  // Invariant: a chosen binary body needs a non-empty file path.
  static Result<Body> binary(std::string filePath) {
    if (filePath.empty())
      return Result<Body>::fail({ErrorCode::Validation, "binary body needs filePath", "body.binary.filePath"});
    return Result<Body>::ok(Body(BodyBinary{std::move(filePath)}));
  }

  template <class V> decltype(auto) match(V &&v) const { return std::visit(std::forward<V>(v), data_); }

  bool isNone() const { return std::holds_alternative<BodyNone>(data_); }

  bool operator==(const Body &o) const { return data_ == o.data_; }
  bool operator!=(const Body &o) const { return !(data_ == o.data_); }

private:
  explicit Body(Variant v) : data_(std::move(v)) {}
  Variant data_;
};

} // namespace core::domain
