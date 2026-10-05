// HTTP transport for the SDK <-> browser-gateway REST API. Owns auth headers,
// the retry policy, and timeouts. Mirrors `Solari.request()` in sdk/src/index.ts.
//
// Retry policy (deliberately narrower than the desktop SDK's): only HTTP
// 502/503/504 and transport errors are retried, with a FIXED backoff (not
// exponential) between attempts. `maxAttempts` counts TOTAL attempts, not
// retries — the default 2 means "one retry".
#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace solari::browser {

/** Region `us-west`. Additional regions are coming; override via ClientOptions. */
inline constexpr const char* kDefaultBaseUrl = "https://api.getsolari.com";
/** Total attempts per request (1 initial + 1 retry). */
inline constexpr int kDefaultMaxAttempts = 2;
/** Fixed pause between attempts, in ms. */
inline constexpr long kDefaultBackoffMs = 500;
/** Per-attempt request timeout, in ms. */
inline constexpr long kDefaultTimeoutMs = 90000;
/** Timeout for the presigned-storageState GET, in ms. */
inline constexpr long kStorageStateTimeoutMs = 8000;

/**
 * Percent-encode exactly like JavaScript's `encodeURIComponent`: everything is
 * escaped except `A-Za-z0-9` and `-_.!~*'()`. Used on path segments so a session
 * or profile id can never break out of its slot.
 */
std::string encodeURIComponent(const std::string& s);

/** The only statuses worth retrying on their own: 502, 503, 504. */
bool isRetryableStatus(int status);

/**
 * Whether a request is safe to send twice. The browser API issues no
 * Idempotency-Key, so the gateway's `retryable` hint is honoured ONLY for
 * these methods — a re-sent POST /sessions could leave a second live session
 * behind.
 */
bool isIdempotentMethod(const std::string& method);

/**
 * Whether THIS REQUEST may be sent again. Per-request, not per-method: a POST
 * is not idempotent by verb, but a POST carrying an `Idempotency-Key` is safe
 * to replay, because the server answers the second copy from the first one's
 * result instead of creating twice.
 *
 * RETRACTED REASON, kept deliberately: this used to be method-only, because
 * the browser API issued no Idempotency-Key and a re-sent POST /sessions could
 * leave a second live session behind. Creates now mint one.
 */
bool isSafeToReplay(const std::string& method, const std::string& idempotencyKey);

/** A key identifies the CALL, not the attempt: minted once, reused by retries. */
std::string newIdempotencyKey();

/**
 * Whether the gateway explicitly marked a response retryable. The flag can
 * appear on a status OUTSIDE the 5xx allowlist — today `404 ReplayPending`,
 * where the recording upload is still in flight.
 */
bool saysRetryable(const std::string& body);

/**
 * Rewrite a Playwright-wire endpoint into its raw-CDP sibling by swapping the
 * `/ws/<id>` path prefix for `/cdp/<id>`. Everything else (scheme, host, query)
 * is preserved. Returns the input unchanged when it doesn't parse or the path
 * isn't `/ws/…` — mirrors `deriveCdpFromWs` in sdk/src/index.ts.
 */
std::string deriveCdpFromWs(const std::string& wsEndpoint);

/** ISO-8601 UTC timestamp `now + offsetMs`, e.g. "2026-07-16T09:30:00.000Z". */
std::string iso8601FromNow(long long offsetMs);

/** The concrete bytes of a request — exposed so tests can assert the shape. */
struct PreparedRequest {
  std::string method;
  std::string url;
  std::vector<std::string> headers;  // "Key: Value"
  std::string body;
  bool hasBody = false;
};

/** A completed HTTP round-trip. Non-2xx is data, not an exception (see request). */
struct HttpResponse {
  int status = 0;
  std::string body;
  bool ok() const { return status >= 200 && status < 300; }
};

class HttpTransport {
 public:
  HttpTransport(std::string apiKey, std::string baseUrl = kDefaultBaseUrl,
                int maxAttempts = kDefaultMaxAttempts,
                long backoffMs = kDefaultBackoffMs,
                long timeoutMs = kDefaultTimeoutMs);

  const std::string& baseUrl() const { return baseUrl_; }
  long timeoutMs() const { return timeoutMs_; }

  /**
   * Build (but do not send) the request bytes. Pure — used by tests.
   * `Content-Type: application/json` is sent on every request, body or not,
   * matching the reference SDK's static header map.
   */
  PreparedRequest prepare(const std::string& method, const std::string& path,
                          bool hasBody, const std::string& body,
                          const std::string& idempotencyKey = "") const;

  /**
   * Perform a request under the retry policy. A non-retryable non-2xx response
   * is RETURNED (callers map status -> SolariError themselves, and DELETE
   * tolerates 404). Throws SolariError only when every attempt failed.
   */
  HttpResponse request(const std::string& method, const std::string& path,
                       const std::optional<nlohmann::json>& body = std::nullopt,
                       const std::string& idempotencyKey = "");

 private:
  struct RawResponse {
    int status = 0;
    std::string body;
    bool networkError = false;
    std::string errMsg;
  };
  RawResponse perform(const PreparedRequest& req) const;

  std::string apiKey_;
  std::string baseUrl_;
  int maxAttempts_;
  long backoffMs_;
  long timeoutMs_;
};

/**
 * GET an absolute URL with NO auth headers and return the body. For presigned
 * S3 links (storageState, replay downloads), which carry their own signature and
 * reject an `Authorization` header. Throws SolariError on transport failure or
 * a non-2xx status.
 */
std::string fetchUrl(const std::string& url, long timeoutMs);

}  // namespace solari::browser
