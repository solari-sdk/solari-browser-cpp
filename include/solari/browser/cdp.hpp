// A MINIMAL raw-CDP WebSocket client — a thin escape hatch, NOT a browser
// automation API.
//
// There is no mature Playwright/Puppeteer equivalent for C++, and this SDK does
// not try to invent one. `CdpConnection` does exactly one thing: send a CDP
// command over `Session::cdpEndpoint` and block for its correlated reply. It has
// no page/frame/element model, no auto-attach, no waiting primitives, no
// navigation lifecycle. If you want those, point a third-party CDP client at
// `session.cdpEndpoint` instead (see README).
//
// Compiled only when SOLARI_WITH_WS=ON.
#pragma once
#ifdef SOLARI_WITH_WS
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

namespace solari::browser {

/**
 * One raw-CDP WebSocket. Not copyable; `send()` is thread-safe.
 *
 *   solari::browser::CdpConnection cdp(session.cdpEndpoint);
 *   cdp.connect();
 *   auto version = cdp.send("Browser.getVersion");
 *   auto targets = cdp.send("Target.getTargets");
 *
 * Attaching to a page target yields a CDP sessionId, which subsequent commands
 * must carry:
 *
 *   auto att = cdp.send("Target.attachToTarget",
 *                       {{"targetId", id}, {"flatten", true}});
 *   cdp.send("Page.navigate", {{"url", "https://example.com"}},
 *            att["sessionId"].get<std::string>());
 */
class CdpConnection {
 public:
  /** `cdpEndpoint` is the session's raw-CDP URL. The signed session id in the
   *  path IS the credential — no auth header is sent (or needed). */
  explicit CdpConnection(std::string cdpEndpoint, long callTimeoutMs = 30000);
  ~CdpConnection();
  CdpConnection(const CdpConnection&) = delete;
  CdpConnection& operator=(const CdpConnection&) = delete;

  /** Open the socket and block until the handshake completes. Throws SolariError. */
  void connect();
  /** Close the socket and fail every in-flight `send()`. Idempotent. */
  void close();
  bool connected() const;

  /**
   * Send one CDP command and block for its reply.
   * Returns the `result` object. Throws SolariError on a protocol `error`, on
   * timeout (`callTimeoutMs`), or when the socket is not open.
   * `sessionId` targets a flattened target session (see class docs).
   */
  nlohmann::json send(const std::string& method,
                      const nlohmann::json& params = nlohmann::json::object(),
                      const std::optional<std::string>& sessionId = std::nullopt);

  /**
   * Subscribe to CDP events (any frame without an `id`, e.g.
   * `Page.loadEventFired`). Invoked on the receive thread — do NOT call `send()`
   * or `close()` from inside it. Set before `connect()`: it is read without a
   * lock, and early events would otherwise be missed.
   */
  void onEvent(std::function<void(const nlohmann::json&)> handler);

 private:
  struct Pending {
    bool done = false;
    nlohmann::json reply;
  };

  void handleMessage(const std::string& text);
  void failAll(const std::string& reason);

  std::string endpoint_;
  long callTimeoutMs_;
  // shared_ptr, not unique_ptr: `send()` copies it under the lock so a
  // concurrent `close()` can't free the socket out from under an in-flight send.
  std::shared_ptr<ix::WebSocket> ws_;
  std::function<void(const nlohmann::json&)> onEvent_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::map<int, std::shared_ptr<Pending>> pending_;
  int nextId_ = 1;
  bool opened_ = false;
  bool failed_ = false;
  std::string errMsg_;
};

}  // namespace solari::browser
#endif  // SOLARI_WITH_WS
