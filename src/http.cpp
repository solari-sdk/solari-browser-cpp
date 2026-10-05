#include "solari/browser/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>
#include <thread>

#include "solari/browser/errors.hpp"

namespace solari::browser {
namespace {

/** Raw outcome of one curl round-trip. */
struct CurlResult {
  int status = 0;
  std::string body;
  bool networkError = false;
  std::string errMsg;
};

std::size_t writeCb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

CurlResult doCurl(const std::string& method, const std::string& url,
                  const std::vector<std::string>& headerLines, bool hasBody,
                  const std::string& body, long timeoutMs) {
  CurlResult out;
  CURL* curl = curl_easy_init();
  if (!curl) {
    out.networkError = true;
    out.errMsg = "curl_easy_init failed";
    return out;
  }
  struct curl_slist* headers = nullptr;
  for (const auto& h : headerLines) headers = curl_slist_append(headers, h.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  if (hasBody) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    out.networkError = true;
    out.errMsg = curl_easy_strerror(rc);
  } else {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    out.status = static_cast<int>(code);
  }
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return out;
}

std::string stripTrailingSlashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

}  // namespace

std::string encodeURIComponent(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '!' || c == '~' || c == '*' ||
                            c == '\'' || c == '(' || c == ')';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

// 507 is listed although THIS gateway does not emit one (censused 2026-09-22:
// browser emits 501/502/503 only). Desktop's InsufficientCapacity 507 is
// transient, and a client's correctness must not depend on which gateway build
// it reaches. Inert today, deliberately — do not remove it as dead code.
bool isRetryableStatus(int status) {
  return status == 502 || status == 503 || status == 504 || status == 507;
}

// Methods safe to send twice. The browser API issues no Idempotency-Key, so
// the `retryable` hint is honoured ONLY for these: a re-sent POST /sessions
// could leave a second live session behind.
std::string newIdempotencyKey() {
  // Dependency-free on purpose: the server treats the key as opaque and scopes
  // it to (org, key), so process-uniqueness is enough. Adding a UUID library to
  // a published package for one string is not worth the supply-chain surface.
  static std::atomic<unsigned long long> counter{0};
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  std::ostringstream os;
  os << "slr-" << std::hex << ns << "-"
     << counter.fetch_add(1, std::memory_order_relaxed) << "-"
     << std::hex << std::random_device{}();
  return os.str();
}

bool isSafeToReplay(const std::string& method, const std::string& idempotencyKey) {
  return isIdempotentMethod(method) || !idempotencyKey.empty();
}

bool isIdempotentMethod(const std::string& method) {
  std::string m;
  m.reserve(method.size());
  for (const char c : method) m.push_back(static_cast<char>(std::toupper(
      static_cast<unsigned char>(c))));
  return m == "GET" || m == "HEAD" || m == "DELETE" || m == "PUT";
}

// Whether the gateway explicitly marked this response retryable. The flag can
// appear on a status OUTSIDE the 5xx allowlist — today `404 ReplayPending`,
// where the recording upload is still in flight. Mirrors the reference SDK's
// `try { JSON.parse(text) } catch {}` + `parsed.retryable === true`.
bool saysRetryable(const std::string& body) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(body);
  } catch (const std::exception&) {
    return false;  // a non-JSON body carries no hint
  }
  if (!parsed.is_object()) return false;
  const auto it = parsed.find("retryable");
  return it != parsed.end() && it->is_boolean() && it->get<bool>();
}

std::string deriveCdpFromWs(const std::string& wsEndpoint) {
  const auto schemeEnd = wsEndpoint.find("://");
  if (schemeEnd == std::string::npos) return wsEndpoint;
  const auto authorityStart = schemeEnd + 3;
  const auto pathStart = wsEndpoint.find('/', authorityStart);
  // No authority (e.g. "wss:///ws/x") or no path at all -> leave it alone.
  if (pathStart == std::string::npos || pathStart == authorityStart) return wsEndpoint;

  static const std::string kWsPrefix = "/ws/";
  if (wsEndpoint.compare(pathStart, kWsPrefix.size(), kWsPrefix) != 0) {
    return wsEndpoint;
  }
  return wsEndpoint.substr(0, pathStart) + "/cdp/" +
         wsEndpoint.substr(pathStart + kWsPrefix.size());
}

std::string iso8601FromNow(long long offsetMs) {
  const auto now = std::chrono::system_clock::now() +
                   std::chrono::milliseconds(offsetMs);
  const auto sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch())
                              .count();
  std::time_t secs = static_cast<std::time_t>(sinceEpoch / 1000);
  const int ms = static_cast<int>(sinceEpoch % 1000);
  std::tm tmv{};
  gmtime_r(&secs, &tmv);
  // Real output is always 24 chars; sized for the compiler's worst case (it
  // can't prove the tm fields are in range) so this stays warning-clean.
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, ms);
  return std::string(buf);
}

HttpTransport::HttpTransport(std::string apiKey, std::string baseUrl,
                             int maxAttempts, long backoffMs, long timeoutMs)
    : apiKey_(std::move(apiKey)),
      baseUrl_(stripTrailingSlashes(std::move(baseUrl))),
      maxAttempts_(std::max(1, maxAttempts)),
      backoffMs_(std::max(0L, backoffMs)),
      timeoutMs_(timeoutMs) {
  if (apiKey_.empty()) throw SolariError("Solari: apiKey is required");
  if (baseUrl_.empty()) throw SolariError("Solari: baseUrl is required");
}

PreparedRequest HttpTransport::prepare(const std::string& method,
                                       const std::string& path, bool hasBody,
                                       const std::string& body,
                                       const std::string& idempotencyKey) const {
  PreparedRequest r;
  r.method = method;
  r.url = baseUrl_ + path;
  r.hasBody = hasBody;
  r.body = body;
  // Mirrors the reference SDK's static header map: both headers, always.
  r.headers.push_back("Authorization: Bearer " + apiKey_);
  r.headers.push_back("Content-Type: application/json");
  if (!idempotencyKey.empty()) {
    r.headers.push_back("Idempotency-Key: " + idempotencyKey);
  }
  return r;
}

HttpTransport::RawResponse HttpTransport::perform(const PreparedRequest& req) const {
  CurlResult c = doCurl(req.method, req.url, req.headers, req.hasBody, req.body,
                        timeoutMs_);
  RawResponse out;
  out.status = c.status;
  out.body = std::move(c.body);
  out.networkError = c.networkError;
  out.errMsg = std::move(c.errMsg);
  return out;
}

HttpResponse HttpTransport::request(const std::string& method,
                                    const std::string& path,
                                    const std::optional<nlohmann::json>& body,
                                    const std::string& idempotencyKey) {
  const bool hasBody = body.has_value();
  // Prepared ONCE, outside the retry loop, so every attempt carries the same key.
  const PreparedRequest req = prepare(
      method, path, hasBody, hasBody ? body->dump() : std::string(), idempotencyKey);

  std::string lastErr;
  for (int attempt = 1; attempt <= maxAttempts_; attempt++) {
    RawResponse res = perform(req);
    if (!res.networkError) {
      // 2xx, or a status we don't retry: hand it back for the caller to map.
      if (res.status >= 200 && res.status < 300) {
        return HttpResponse{res.status, std::move(res.body)};
      }
      const bool retryThis =
          isRetryableStatus(res.status) ||
          (isSafeToReplay(method, idempotencyKey) && saysRetryable(res.body));
      if (!retryThis) {
        return HttpResponse{res.status, std::move(res.body)};
      }
      lastErr = "Solari " + method + " " + path + ": " + std::to_string(res.status);
    } else {
      lastErr = res.errMsg;
    }

    if (attempt < maxAttempts_) {
      // FIXED backoff — not exponential (mirrors the reference SDK).
      std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs_));
    }
  }

  throw SolariError("Solari " + method + " " + path + ": exhausted " +
                    std::to_string(maxAttempts_) + " attempts (" + lastErr + ")");
}

std::string fetchUrl(const std::string& url, long timeoutMs) {
  CurlResult c = doCurl("GET", url, {}, false, "", timeoutMs);
  if (c.networkError) {
    throw SolariError("Solari: GET " + url + " failed: " + c.errMsg);
  }
  if (c.status < 200 || c.status >= 300) {
    throw SolariError("Solari: GET returned " + std::to_string(c.status) + ": " +
                          c.body.substr(0, 256),
                      c.status);
  }
  return c.body;
}

}  // namespace solari::browser
