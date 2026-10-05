// `Client` — the Solari browser CONTROL PLANE: create/inspect/release sessions,
// manage stored profiles, fetch replays, list proxy countries. Ported from the
// `Solari` class in sdk/src/index.ts.
//
// SCOPE: control plane only. The TS SDK's `launch()` returns a live Playwright
// `Browser`; there is no Playwright for C++, so this binding stops at handing
// back `Session::cdpEndpoint`. Drive it with a third-party CDP client (see
// README), or with the optional `CdpConnection` escape hatch (SOLARI_WITH_WS).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "solari/browser/http.hpp"
#include "solari/browser/types.hpp"

namespace solari::browser {

/**
 * Build the `POST /sessions` JSON body: falsy/unset fields are omitted, so an
 * empty options struct yields an empty object (the caller then sends NO body).
 * Public so tests can assert the request shape without a live gateway.
 */
nlohmann::json buildCreateBody(const CreateSessionOptions& opts);

/**
 * Decide whether a `DELETE /sessions/:id` response means the release FAILED.
 *
 * A BARE 404 is success — the session is already gone. A 404 the gateway marked
 * `InvalidSessionId` is a FAILURE: the gateway acks 204 for any authentic
 * session id, including one whose session has already ended (the handler is
 * idempotent by design and never consults the pool before acking). So a 404
 * does not mean "already released"; it means the gateway refused the id
 * (malformed, forged, or another org's) and released nothing, leaving the pool
 * slot held until orphan-grace. Treating that as success is what made these
 * releases leak slots silently.
 *
 * Pure, and public so tests can assert the rule without a live gateway.
 * Mirrors `releaseRejection` in sdk/src/index.ts.
 */
bool releaseRejected(int status, const std::string& body);

/**
 * Parse a `201 POST /sessions` body into a `Session`. Pure — the presigned
 * storageState fetch is left to the caller. Derives `cdpEndpoint` from
 * `wsEndpoint` when the gateway omits it, and defaults `expiresAt` to one hour
 * out. Throws SolariError when `sessionId` / `wsEndpoint` are missing.
 */
Session parseSessionResponse(const nlohmann::json& body);

/** Parse `GET /sessions/:id/replay-url` (expiresInSeconds -> 0, contentEncoding -> "gzip"). */
ReplayUrl parseReplayUrl(const nlohmann::json& body);

/** Parse one profile row; `raw` keeps the whole row the platform returned. */
Profile parseProfile(const nlohmann::json& row);

/** Parse a `GET /profiles` array. Non-object rows are skipped. */
std::vector<Profile> parseProfiles(const nlohmann::json& body);

/** Parse `POST /profiles/:id/save` (version / sizeBytes -> 0 when omitted). */
SaveProfileResult parseSaveResult(const nlohmann::json& body);

/** Parse `GET /proxy/countries`. Non-string country entries are skipped. */
ProxyCountries parseProxyCountries(const nlohmann::json& body);

class Client;

/** `client.sessions.*` */
class SessionsResource {
 public:
  explicit SessionsResource(Client* client) : client_(client) {}

  /** `POST /sessions` -> a live session. */
  Session create(const CreateSessionOptions& opts = {});

  /**
   * `GET /sessions/:id`, returned as raw JSON.
   *
   * NOTE: the gateway proxies this straight through to the pool host, which
   * currently implements no `GET /sessions/:id` route — so this reliably 404s
   * today (SolariError, status 404). Kept for completeness; the shape is
   * whatever the pool eventually returns, hence raw JSON.
   */
  Json get(const std::string& id);

  /** `DELETE /sessions/:id`. Idempotent: a 404 is success (already gone). */
  void release(const std::string& id);

  /** `GET /sessions/:id/replay-url`. Available ~1-3s after `release`; 404 until then. */
  ReplayUrl getReplayUrl(const std::string& id);

  /** Fetch the replay bytes (gzipped NDJSON) from the presigned URL. */
  std::string downloadReplay(const std::string& id);

 private:
  Client* client_;
};

/** `client.profiles.*` */
class ProfilesResource {
 public:
  explicit ProfilesResource(Client* client) : client_(client) {}

  /** `GET /profiles` — every profile owned by the caller's org. */
  std::vector<Profile> list();
  /** `POST /profiles` `{name}`. */
  Profile create(const std::string& name);
  /** `DELETE /profiles/:id`. Idempotent: a 404 is success. (`delete` is a keyword.) */
  void remove(const std::string& id);
  /** `POST /profiles/:id/save` `{storageState}` — persist cookies/localStorage. */
  SaveProfileResult save(const std::string& id, const StorageState& storageState);

 private:
  Client* client_;
};

/**
 * The API client. Blocking and synchronous; safe to share between threads only
 * insofar as libcurl's easy handles are created per request.
 */
class Client {
 public:
  explicit Client(const ClientOptions& options);
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  SessionsResource sessions;
  ProfilesResource profiles;

  /** `GET /proxy/countries` — supported egress countries + whether proxying is configured. */
  ProxyCountries proxyCountries();

  HttpTransport& http() { return *http_; }

 private:
  std::shared_ptr<HttpTransport> http_;
};

}  // namespace solari::browser
