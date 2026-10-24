#include "solari/browser/client.hpp"
#ifdef SOLARI_WITH_WS

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "solari/browser/cdp.hpp"
#include "solari/browser/errors.hpp"

namespace solari::browser {

LaunchedSession::LaunchedSession(Client* client, Session session,
                                 std::unique_ptr<CdpConnection> cdp)
    : client_(client), session_(std::move(session)), cdp_(std::move(cdp)) {}

LaunchedSession::~LaunchedSession() {
  try {
    close();
  } catch (...) {
    // Best effort: a destructor must not throw. The gateway's orphan reaper
    // frees the slot if this release did not land.
  }
}

void LaunchedSession::close() {
  if (released_) return;
  if (cdp_) cdp_->close();
  client_->sessions.release(session_.id);
  released_ = true;
}

bool isTransientLaunchError(const SolariError& e) {
  if (!e.status) return true;
  if (e.code && *e.code == "PoolResponseUnparseable") return false;
  return *e.status >= 500;
}

nlohmann::json cookiesForCdp(const StorageState& state) {
  nlohmann::json out = nlohmann::json::array();
  if (!state.is_object()) return out;
  auto it = state.find("cookies");
  if (it == state.end() || !it->is_array()) return out;
  for (const auto& c : *it) {
    if (!c.is_object()) continue;
    auto name = c.find("name");
    auto value = c.find("value");
    if (name == c.end() || !name->is_string() || value == c.end() || !value->is_string()) continue;
    nlohmann::json o = {{"name", *name}, {"value", *value}};
    for (const char* k : {"domain", "path"}) {
      auto f = c.find(k);
      if (f != c.end() && f->is_string() && !f->get<std::string>().empty()) o[k] = *f;
    }
    // Playwright uses -1 for a session cookie: leave `expires` unset then.
    auto ex = c.find("expires");
    if (ex != c.end() && ex->is_number() && ex->get<double>() > 0) o["expires"] = *ex;
    for (const char* k : {"httpOnly", "secure"}) {
      auto f = c.find(k);
      if (f != c.end() && f->is_boolean()) o[k] = *f;
    }
    auto ss = c.find("sameSite");
    if (ss != c.end() && ss->is_string()) {
      const std::string v = ss->get<std::string>();
      if (v == "Strict" || v == "Lax" || v == "None") o["sameSite"] = v;
    }
    out.push_back(std::move(o));
  }
  return out;
}

std::unique_ptr<LaunchedSession> Client::launch(const LaunchOptions& opts) {
  const int totalAttempts = (opts.retries > 0 ? opts.retries : 0) + 1;
  const bool probe = opts.probe.value_or(opts.retries > 0);

  for (int attempt = 1;; attempt++) {
    std::optional<Session> session;
    std::unique_ptr<CdpConnection> cdp;
    try {
      session = sessions.create(opts.create);
      cdp = std::make_unique<CdpConnection>(session->cdpEndpoint, opts.callTimeoutMs);
      cdp->connect();

      if (session->storageState) {
        const nlohmann::json cookies = cookiesForCdp(*session->storageState);
        if (!cookies.empty()) {
          try {
            cdp->send("Storage.setCookies", {{"cookies", cookies}});
          } catch (const SolariError&) {
            // Seeding is best effort: a refused cookie must not lose the session.
          }
        }
      }
      if (probe) {
        cdp->setCallTimeoutMs(opts.probeTimeoutMs);
        cdp->send("Browser.getVersion");
        cdp->setCallTimeoutMs(opts.callTimeoutMs);
      }
      return std::make_unique<LaunchedSession>(this, std::move(*session), std::move(cdp));
    } catch (const SolariError& e) {
      // Whatever got created is released before the next attempt or the throw.
      if (cdp) cdp->close();
      if (session) {
        try {
          sessions.release(session->id);
        } catch (const SolariError&) {
        }
      }
      if (attempt >= totalAttempts || !isTransientLaunchError(e)) throw;
      std::this_thread::sleep_for(std::chrono::milliseconds(opts.retryBackoffMs));
    }
  }
}

}  // namespace solari::browser
#endif  // SOLARI_WITH_WS
