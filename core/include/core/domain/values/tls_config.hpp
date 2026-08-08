#pragma once

#include <string>
#include <utility>

namespace core::domain {

class TlsConfig {
public:
  static TlsConfig create(bool enabled, bool insecureSkipVerify, std::string caCertPath,
                          std::string clientCertPath, std::string clientKeyPath) {
    return TlsConfig(enabled, insecureSkipVerify, std::move(caCertPath), std::move(clientCertPath),
                     std::move(clientKeyPath));
  }
  static TlsConfig disabled() { return TlsConfig(false, false, {}, {}, {}); }

  bool enabled() const noexcept { return enabled_; }
  bool insecureSkipVerify() const noexcept { return insecureSkipVerify_; }
  const std::string &caCertPath() const noexcept { return caCertPath_; }
  const std::string &clientCertPath() const noexcept { return clientCertPath_; }
  const std::string &clientKeyPath() const noexcept { return clientKeyPath_; }

  // Soft warning (not enforced): a client certificate usually needs its private key.
  bool hasClientCertWithoutKey() const { return !clientCertPath_.empty() && clientKeyPath_.empty(); }

  bool operator==(const TlsConfig &o) const {
    return enabled_ == o.enabled_ && insecureSkipVerify_ == o.insecureSkipVerify_ &&
           caCertPath_ == o.caCertPath_ && clientCertPath_ == o.clientCertPath_ &&
           clientKeyPath_ == o.clientKeyPath_;
  }
  bool operator!=(const TlsConfig &o) const { return !(*this == o); }

private:
  TlsConfig(bool enabled, bool insecureSkipVerify, std::string caCertPath, std::string clientCertPath,
            std::string clientKeyPath)
      : enabled_(enabled), insecureSkipVerify_(insecureSkipVerify), caCertPath_(std::move(caCertPath)),
        clientCertPath_(std::move(clientCertPath)), clientKeyPath_(std::move(clientKeyPath)) {}

  bool enabled_;
  bool insecureSkipVerify_;
  std::string caCertPath_;
  std::string clientCertPath_;
  std::string clientKeyPath_;
};

} // namespace core::domain
