// soap_test.cpp — SOAP -> HTTP packaging (version header policy), faultstring extraction, and the
// conservative XML indenter (SPEC_soap §4/§5). Pure fixtures, no network.
#include <cstdio>
#include <string>

#include "core/infra/serialization/field_json.hpp"
#include "infra/transport/soap/native_soap_sender.hpp"

using namespace core;
namespace d = core::domain;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char *msg) {
  if (ok) ++g_pass;
  else { ++g_fail; std::printf("  FAIL[soap]: %s\n", msg); }
}
bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

d::RequestModel soapModel(d::SoapVersion v, const std::string &action,
                          std::vector<d::Header> extraHeaders = {}, d::Auth auth = d::Auth::none()) {
  d::SoapRequest::Parts p{d::Url::create("http://svc.test/calc").take()};
  p.action = action;
  p.version = v;
  p.envelope = "<Envelope/>";
  p.headers = d::HeaderList(std::move(extraHeaders));
  p.auth = std::move(auth);
  d::RequestConfig cfg{d::Timeout::fromMillis(1000).take(), true};
  return d::RequestModel::create(d::RequestId("s1"), "S", 0, cfg,
                                 d::SoapRequest::create(std::move(p)).take())
      .take();
}

std::string headerValue(const d::HttpRequest &h, const char *name) {
  for (const auto &hd : h.headers().items())
    if (hd.name() == name) return hd.value();
  return {};
}
} // namespace

int run_soap_tests() {
  // buildSoapHttpModel — 1.1 headers.
  {
    auto m = infra::soap::buildSoapHttpModel(soapModel(d::SoapVersion::V1_1, "urn:Add"));
    const auto &h = std::get<d::HttpRequest>(m.payload());
    check(h.method() == d::HttpMethod::Post, "1.1: POST");
    check(headerValue(h, "Content-Type") == "text/xml; charset=utf-8", "1.1: text/xml content type");
    check(headerValue(h, "SOAPAction") == "\"urn:Add\"", "1.1: SOAPAction quoted");
    bool xmlBody = false;
    h.body().match([&](auto &&b) {
      if constexpr (std::is_same_v<std::decay_t<decltype(b)>, d::BodyRaw>)
        xmlBody = b.subtype == d::RawSubtype::Xml && b.text == "<Envelope/>";
    });
    check(xmlBody, "1.1: body = raw XML envelope");
  }
  // 1.1 with EMPTY action still sends SOAPAction: "" (spec-mandatory header).
  {
    auto m = infra::soap::buildSoapHttpModel(soapModel(d::SoapVersion::V1_1, ""));
    const auto &h = std::get<d::HttpRequest>(m.payload());
    check(headerValue(h, "SOAPAction") == "\"\"", "1.1: empty action -> SOAPAction \"\"");
  }
  // 1.2: action rides in the Content-Type, no SOAPAction header.
  {
    auto m = infra::soap::buildSoapHttpModel(soapModel(d::SoapVersion::V1_2, "urn:Add"));
    const auto &h = std::get<d::HttpRequest>(m.payload());
    check(headerValue(h, "Content-Type") == "application/soap+xml; charset=utf-8; action=\"urn:Add\"",
          "1.2: soap+xml content type with action");
    check(headerValue(h, "SOAPAction").empty(), "1.2: no SOAPAction header");
  }
  // User-set Content-Type / SOAPAction win (no double headers).
  {
    std::vector<d::Header> hs;
    hs.push_back(d::Header::create("Content-Type", "text/xml; charset=iso-8859-1").take());
    hs.push_back(d::Header::create("SOAPAction", "\"urn:Custom\"").take());
    auto m = infra::soap::buildSoapHttpModel(soapModel(d::SoapVersion::V1_1, "urn:Add", std::move(hs)));
    const auto &h = std::get<d::HttpRequest>(m.payload());
    int ctCount = 0, saCount = 0;
    for (const auto &hd : h.headers().items()) {
      if (hd.name() == "Content-Type") ++ctCount;
      if (hd.name() == "SOAPAction") ++saCount;
    }
    check(ctCount == 1 && headerValue(h, "Content-Type") == "text/xml; charset=iso-8859-1",
          "user Content-Type wins");
    check(saCount == 1 && headerValue(h, "SOAPAction") == "\"urn:Custom\"", "user SOAPAction wins");
  }
  // Auth carried through to the HTTP model (Bearer here; OAuth2 materializes earlier in the saga).
  {
    auto m = infra::soap::buildSoapHttpModel(
        soapModel(d::SoapVersion::V1_1, "", {}, d::Auth::bearer("tok").take()));
    check(std::get<d::HttpRequest>(m.payload()).auth() == d::Auth::bearer("tok").take(),
          "auth carried into HTTP model");
  }

  // extractFaultString.
  {
    check(infra::soap::extractFaultString(
              "<soap:Fault><faultcode>x</faultcode><faultstring>Divide by zero</faultstring></soap:Fault>") ==
              "Divide by zero",
          "1.1 faultstring extracted");
    check(infra::soap::extractFaultString(
              "<env:Fault><env:Reason><env:Text xml:lang=\"en\">Bad input</env:Text></env:Reason></env:Fault>") ==
              "Bad input",
          "1.2 Reason/Text extracted");
    check(infra::soap::extractFaultString("<ok/>").empty(), "no fault -> empty");
  }

  // formatXml (SPEC_soap §5) — conservative indenter.
  {
    std::string in = "<a><b attr=\"1\">x</b><c/></a>";
    std::string out = serial::formatXml(in);
    check(has(out, "<a>\n  <b attr=\"1\">x</b>\n  <c/>\n</a>"), "nested indent + inline text + self-close");
    check(serial::formatXml("<a><![CDATA[x]]></a>") == "<a><![CDATA[x]]></a>", "CDATA -> verbatim");
    check(serial::formatXml("<a><b></a>") == "<a><b></a>", "mismatched -> verbatim");
    check(serial::formatXml("not xml") == "not xml", "non-XML -> verbatim");
    check(has(serial::formatJson("<a><b>1</b></a>", true), "\n  <b>"), "formatJson falls back to XML pretty");
  }

  std::printf("  soap: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
