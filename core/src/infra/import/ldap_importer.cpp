#include <cstdlib>
#include <string>
#include <vector>

#include "core/infra/import/importer.hpp"
#include "infra/import/shell_tokenize.hpp"
#include "infra/transport/shared/url_util.hpp" // RFC 4516 URLs are percent-encoded

namespace core {

namespace d = core::domain;

namespace {

// Mutable parse scratch only — not the persisted model.
struct SLdap {
    std::string url, host, port, bindDn, bindPassword, baseDn, filter, group, scope = "sub";
    std::vector<std::string> attributes;
    bool startTls = false, ldaps = false;
    int sizeLimit = 100, timeLimit = 10, pageSize = 500;
};

int toInt(const std::string& s, int dflt) {
    if (s.empty()) return dflt;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || v < 0 || v > 1000000) return dflt;
    return static_cast<int>(v);
}

// ldapsearch flags that consume the NEXT token as their value.
bool ldapFlagTakesValue(const std::string& tk) {
    return tk == "-H" || tk == "-D" || tk == "-w" || tk == "-b" || tk == "-s" || tk == "-z" ||
           tk == "-l" || tk == "-h" || tk == "-p" || tk == "-E" || tk == "-y" || tk == "-o";
}

// -E [!]pr=<size>[/noprompt] -> pageSize. Any other extension is reported as unknown.
bool applyExtension(const std::string& v, SLdap& s) {
    std::string e = v;
    if (!e.empty() && e[0] == '!') e = e.substr(1);   // criticality marker
    if (lower(e).rfind("pr=", 0) != 0) return false;
    std::string n = e.substr(3);
    if (auto slash = n.find('/'); slash != std::string::npos) n = n.substr(0, slash); // strip /noprompt
    s.pageSize = toInt(n, s.pageSize);
    return true;
}

void applyLdapValueFlag(const std::string& flag, const std::string& v, SLdap& s,
                        std::vector<std::string>& unknown) {
    if (flag == "-H") s.url = v;
    else if (flag == "-D") s.bindDn = v;
    else if (flag == "-w") s.bindPassword = v;
    else if (flag == "-b") s.baseDn = v;
    else if (flag == "-s") s.scope = lower(v);
    else if (flag == "-z") s.sizeLimit = toInt(v, s.sizeLimit);
    else if (flag == "-l") s.timeLimit = toInt(v, s.timeLimit);
    else if (flag == "-h") s.host = v;                 // legacy host/port (pre -H)
    else if (flag == "-p") s.port = v;
    else if (flag == "-E") { if (!applyExtension(v, s)) unknown.push_back(flag + " " + v); }
    else unknown.push_back(flag + " " + v);            // -y (password file) / -o: nothing to map
}

// ldapsearch [-x] [-ZZ] [-H uri] [-D dn] [-w pw] [-b base] [-s scope] [-z n] [-l n] filter [attrs...]
void parseLdapsearch(const std::vector<std::string>& tokens, SLdap& s,
                     std::vector<std::string>& unknown) {
    std::vector<std::string> positionals;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& tk = tokens[i];
        if (tk == "-Z" || tk == "-ZZ") s.startTls = true;
        else if (tk == "-x" || tk == "-LLL" || tk == "-n" || tk == "-v" || tk == "-c") continue; // no-ops here
        else if (ldapFlagTakesValue(tk)) {
            if (i + 1 < tokens.size()) applyLdapValueFlag(tk, tokens[++i], s, unknown);
        } else if (!tk.empty() && tk[0] == '-') unknown.push_back(tk);
        else positionals.push_back(tk);
    }
    if (!positionals.empty()) {
        s.filter = positionals[0];                                     // first positional = filter
        for (size_t i = 1; i < positionals.size(); ++i)
            if (positionals[i] != "*" && positionals[i] != "+") s.attributes.push_back(positionals[i]);
    }
    // Legacy -h/-p when no -H was given.
    if (s.url.empty() && !s.host.empty())
        s.url = "ldap://" + s.host + ":" + (s.port.empty() ? "389" : s.port);
}

// RFC 4516: ldap[s]://host:port/baseDn?attr1,attr2?scope?filter  (every part after host optional).
void parseLdapUrl(const std::string& raw, SLdap& s) {
    std::string rest = raw;
    if (lower(rest).rfind("ldaps://", 0) == 0) { s.ldaps = true; rest = rest.substr(8); }
    else if (lower(rest).rfind("ldap://", 0) == 0) rest = rest.substr(7);

    std::string hostport = rest, tail;
    if (auto slash = rest.find('/'); slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        tail = rest.substr(slash + 1);
    }
    if (hostport.find(':') == std::string::npos && !hostport.empty())
        hostport += s.ldaps ? ":636" : ":389";
    s.url = std::string(s.ldaps ? "ldaps://" : "ldap://") + hostport;

    std::vector<std::string> parts;
    std::string cur;
    for (char c : tail) {
        if (c == '?') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    auto part = [&](size_t i) { return i < parts.size() ? urlutil::urlDecode(parts[i]) : std::string(); };

    s.baseDn = part(0);
    std::string attrs = part(1);
    if (!attrs.empty()) {
        std::string a;
        for (char c : attrs + ",") {
            if (c == ',') { if (!a.empty() && a != "*") s.attributes.push_back(trim(a)); a.clear(); }
            else a += c;
        }
    }
    std::string sc = lower(part(2));
    if (sc == "base" || sc == "one" || sc == "sub") s.scope = sc;
    s.filter = part(3);
}

d::RequestModel buildLdapDomain(const SLdap& s) {
    d::LdapRequest::Parts p{d::Url::create(s.url).take()};
    p.startTls = s.startTls;
    p.bindDn = s.bindDn;
    p.bindPassword = s.bindPassword;
    p.baseDn = s.baseDn;
    if (!d::parseLdapScope(s.scope, p.scope)) p.scope = d::LdapScope::Sub;
    p.filter = s.filter.empty() ? "(objectClass=*)" : s.filter;
    p.attributes = s.attributes;
    p.sizeLimit = s.sizeLimit;
    p.timeLimit = s.timeLimit;
    p.pageSize = s.pageSize;
    // startTls + ldaps is rejected by the VO (already TLS) — the URL wins, drop the redundant flag.
    if (s.ldaps) p.startTls = false;
    auto ldap = d::LdapRequest::create(std::move(p)).take();
    d::RequestConfig cfg{d::Timeout::fromMillis(30000).take(), true};
    return d::RequestModel::create(d::RequestId(""), "Imported LDAP", 0, cfg, ldap).take();
}

} // namespace

bool LdapImporter::canHandle(const std::string& input) const {
    std::string t = lower(trim(input.substr(0, 16)));   // prefix only; avoid copying a huge paste
    return t.rfind("ldapsearch", 0) == 0 || t.rfind("ldap://", 0) == 0 || t.rfind("ldaps://", 0) == 0;
}

ImportParseResult LdapImporter::parse(const std::string& input) const {
    SLdap s;
    std::vector<std::string> unknown;
    std::string trimmed = trim(input);
    if (lower(trimmed).rfind("ldapsearch", 0) == 0) parseLdapsearch(shellTokenize(trimmed), s, unknown);
    else parseLdapUrl(trimmed, s);

    if (s.url.empty()) return {false, std::nullopt, {}, "missing ldap host (-H uri or -h host)"};
    if (lower(s.url).rfind("ldaps://", 0) == 0) s.ldaps = true;
    return {true, buildLdapDomain(s), unknown, ""};
}

} // namespace core
