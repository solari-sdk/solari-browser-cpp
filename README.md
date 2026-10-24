# Solari Browser C++ SDK (control plane)

A C++17 binding for the Solari **browser** gateway. It mirrors the control-plane
half of `@solarisdk/browser`: a `solari::browser::Client` with

- `.sessions` — `create` / `get` / `list` / `release` / `getReplayUrl` / `downloadReplay`
- `.profiles` — `list` / `create` / `remove` / `save`
- `.proxyCountries()` — supported egress countries + whether proxying is configured
- `.launch()` — create + raw-CDP connect + health probe + release on close (needs
  `SOLARI_WITH_WS`, see below)

Feature coverage across all five languages is on the
[SDK feature matrix](https://docs.getsolari.com/sdk-capabilities).

The API is **blocking and synchronous**; every call is a REST round-trip via
libcurl.

## Scope: control plane plus a raw-CDP `launch()`

The TypeScript SDK's `launch()` returns a live Playwright `Browser`. **That is not
portable to C++**: Playwright has no C++ client, and neither does Puppeteer. This
binding creates the session and hands back `Session::cdpEndpoint`, and you drive
the browser from there. `client.launch()` (below) is the raw-CDP counterpart: it
returns a connected `CdpConnection`, not a `Browser`.

Two other deliberate differences from the TS SDK:

- **Endpoints are UPSTREAM.** The TS SDK starts a `LocalProxy` and hands back
  loopback URLs; this binding returns the gateway URLs verbatim. Nothing is
  rewritten, and there is no background listener to shut down.
- **`sessions.release()` blocks.** The TS SDK has a fire-and-forget `release()`
  plus a `releaseAndWait()`; here there is one blocking `release()` (i.e.
  `releaseAndWait` semantics). A 404 is treated as success — already gone.

### Driving the browser

`session.cdpEndpoint` speaks raw Chrome DevTools Protocol over a WebSocket. It's a
capability URL — the session id in the path is HMAC-signed, so no `Authorization`
header is needed on the upgrade. **Treat it as a secret.** Anything that speaks CDP
over a `ws://` URL can drive it.

Be warned that C++'s CDP ecosystem is thin: the available clients are small,
mostly unmaintained, and none is a Playwright equivalent — evaluate one yourself
before depending on it rather than taking a recommendation here. **For anything
non-trivial the better shape is to split the process:** drive the browser from a
language that has a real Playwright/Puppeteer client (TypeScript, Python, Go) and
use this SDK for the C++ side of session/profile lifecycle. Solari's stealth,
proxy escalation, captcha solving and recording are all applied server-side, so
nothing is lost by doing so.

`session.wsEndpoint` (the Playwright wire protocol) is returned for completeness
but is only usable by a Playwright client — not by a raw CDP client.

### Durations are milliseconds

Every duration option is an integer in milliseconds (`timeoutMs`, `backoffMs`,
`sessionTimeoutMs`, `probeTimeoutMs`, `callTimeoutMs`). `sessionTimeoutMs` is rounded
up (1001 ms is 2 s); zero or unset leaves the plan maximum and a negative value
throws. **Deprecated:** `CreateSessionOptions::timeoutSeconds` still works; use
`sessionTimeoutMs` (it is documented as deprecated rather than marked
`[[deprecated]]`, because GCC then warns on every `CreateSessionOptions` you construct). When both are set the new one wins. It will not
be removed before the next major release.

### One-call `launch()` (needs `SOLARI_WITH_WS`)

```cpp
solari::browser::LaunchOptions lo;
lo.create.stealth = true;
lo.retries = 2;                       // extra attempts on a transient failure (default 0)
auto launched = client.launch(lo);    // create + connect + seed cookies + probe

auto version = launched->cdp().send("Browser.getVersion");
// ... drive it with launched->cdp() ...
launched->close();                    // closes the socket and releases the session
```

`launch()` creates the session, opens a `CdpConnection` to it, seeds the cookies of
an attached profile (best effort), and, when `retries > 0` or `lo.probe = true`,
sends `Browser.getVersion` within `probeTimeoutMs` (default 2000) to confirm the
browser answers. A session that fails after it was created is released before the
next attempt or the final throw. Only transient failures are retried: no HTTP
status, or a 5xx other than `PoolResponseUnparseable` (the pool may already have
created that session). `LaunchedSession`'s destructor releases the session too
(best effort, never throws), so leaving scope cannot leak a pool slot; the `Client`
must outlive it.

### The `CdpConnection` escape hatch (optional)

When built with `SOLARI_WITH_WS=ON` (the default), the SDK ships a **minimal**
raw-CDP client. It sends a command and blocks for the correlated reply. That's all
it is: **an escape hatch, not a browser automation API.** No page/frame/element
model, no auto-attach, no waiting primitives, no navigation lifecycle — if you
need those, use a real CDP library per above.

```cpp
solari::browser::CdpConnection cdp(session.cdpEndpoint);
cdp.connect();

auto version = cdp.send("Browser.getVersion");
std::cout << version["product"].get<std::string>() << "\n";

// Attach to a page target; flattened sessions carry a CDP sessionId.
auto targets = cdp.send("Target.getTargets");
auto targetId = targets["targetInfos"][0]["targetId"].get<std::string>();
auto att = cdp.send("Target.attachToTarget", {{"targetId", targetId}, {"flatten", true}});
auto sid = att["sessionId"].get<std::string>();

cdp.send("Page.enable", {}, sid);
cdp.send("Page.navigate", {{"url", "https://example.com"}}, sid);
cdp.close();
```

## Layout

```
include/solari/browser/*.hpp   public headers (umbrella: solari/browser/browser.hpp)
src/*.cpp                      implementation
tests/test_main.cpp            offline doctest suite (no gateway needed)
CMakeLists.txt                 FetchContent deps + library + test target
```

## Dependencies

Pulled automatically via CMake `FetchContent`:

- [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` — JSON
- [IXWebSocket](https://github.com/machinezone/IXWebSocket) `v11.4.5` — the
  optional raw-CDP transport, TLS via OpenSSL (`USE_TLS=ON`, `USE_OPEN_SSL=ON`)
- [doctest](https://github.com/doctest/doctest) `v2.4.11` — tests

Required from the system:

- **libcurl** (`find_package(CURL REQUIRED)`) — REST transport
- a threads library (`find_package(Threads REQUIRED)`)
- OpenSSL — for IXWebSocket TLS

On Debian/Ubuntu: `sudo apt-get install libcurl4-openssl-dev libssl-dev`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options:

- `-DSOLARI_WITH_WS=OFF` — skip IXWebSocket. The whole control plane still builds
  and passes its tests; only `CdpConnection` is omitted.
- `-DSOLARI_BUILD_TESTS=OFF` — skip the test target.

Link against the `solari_browser` target (or `libsolari_browser.a` + its headers).

> This project is `solari_browser` / target `solari_browser` / namespace
> `solari::browser`, deliberately distinct from the desktop SDK's `solari_sdk` /
> `solari` / `solari::` — the two can coexist in one build.

## Usage

```cpp
#include <iostream>
#include <solari/browser/browser.hpp>

namespace sb = solari::browser;

int main() {
  sb::ClientOptions opts;
  opts.apiKey = "slr_live_...";        // required; no env-var fallback
  // opts.baseUrl = "http://localhost:4000";   // staging / self-hosted override
  sb::Client client(opts);

  // Create a stealth session with mobile egress out of the UK.
  sb::CreateSessionOptions o;
  o.stealth = true;                    // required for proxy + captcha
  o.recording = true;
  sb::ProxyRequest proxy;
  proxy.country = "gb";
  proxy.tier = "mobile";
  o.proxy = proxy;                     // or: o.proxy = std::string("smart");

  sb::Session session = client.sessions.create(o);
  std::cout << "cdp: " << session.cdpEndpoint << "\n"
            << "expires: " << session.expiresAt << "\n";
  if (session.proxy) std::cout << "egress: " << session.proxy->country << "\n";

  // ... drive session.cdpEndpoint with a CDP client (see above) ...

  client.sessions.release(session.id);

  // The replay lands ~1-3s after release; a 404 until then is expected.
  sb::ReplayUrl replay = client.sessions.getReplayUrl(session.id);
  std::cout << replay.url << " (" << replay.contentEncoding << ")\n";
  return 0;
}
```

### Profiles

```cpp
sb::Profile p = client.profiles.create("logged-in");

// Attach it to a session; cookies/localStorage come back on the session.
sb::CreateSessionOptions o;
o.profileId = p.id;
sb::Session s = client.sessions.create(o);

// storageState is a tri-state:
if (!s.storageState)                 std::cout << "no profile attached\n";
else if (s.storageState->is_null())  std::cout << "profile is empty\n";
else                                 std::cout << "profile has cookies\n";

// Persist state back onto the profile (storageState is raw JSON).
sb::SaveProfileResult saved = client.profiles.save(p.id, *s.storageState);
std::cout << "v" << saved.version << " " << saved.sizeBytes << " bytes\n";

for (const auto& row : client.profiles.list())
  std::cout << row.id << " " << row.name << " " << row.raw.dump() << "\n";

client.profiles.remove(p.id);        // `remove`, since `delete` is a keyword
```

### Proxy countries

```cpp
sb::ProxyCountries c = client.proxyCountries();
if (!c.enabled) std::cout << "this gateway has no proxy credentials\n";
for (const auto& code : c.countries) std::cout << code << "\n";
```

## Wire contract

| | |
|---|---|
| Base URL | `https://api.getsolari.com` (region `us-west`), override via `ClientOptions::baseUrl` |
| Auth | `Authorization: Bearer slr_live_<id>_<secret>` + `Content-Type: application/json` |
| Retries | **only** 502/503/504 and transport errors |
| Attempts | `maxAttempts = 2` **total** (1 retry), `backoffMs = 500` **fixed** (not exponential) |
| Timeout | `timeoutMs = 90000` per attempt; the presigned storageState GET uses 8s |

Endpoints: `POST /sessions`, `GET /sessions/:id`, `DELETE /sessions/:id`,
`GET /sessions/:id/replay-url`, `GET /profiles`, `POST /profiles`,
`DELETE /profiles/:id`, `POST /profiles/:id/save`, `GET /proxy/countries`.

Two notes on the session response:

- **`cdpEndpoint` is derived when absent** by rewriting the `wsEndpoint` path
  `/ws/<id>` → `/cdp/<id>` (`deriveCdpFromWs`, byte-for-byte equivalent to the TS
  original).
- **`storageState` arrives as a presigned URL** (`storageStateUrl.url`), which
  `sessions.create()` fetches for you with an 8s timeout.

- **`sessions.get()` returns raw JSON**: `id`, `status`, `kind`, `org`, `createdAt`,
  `expiresAt`, `userMetadata` when set, and `wsEndpoint` / `cdpEndpoint` while the
  session is active. It is left raw so a gateway field addition never forces an
  SDK bump.
- **`sessions.list()`** pages with `ListSessionsOptions::limit` / `cursor` and
  filters with `metadata` (string values matched against `userMetadata`). Rows never
  carry the websocket endpoints (they are bearer capabilities): call `get(id)`.
- **`CreateSessionOptions`** also takes `sessionTimeoutMs` (milliseconds; rounded up
  to whole seconds for the gateway, which refuses under 60 s), `keepAlive` (hold the session if the client drops, until its deadline or release)
  and `userMetadata` (a JSON object stored with the session).

## Errors

Every failure throws `solari::browser::SolariError` (a `std::runtime_error`)
carrying an optional HTTP `status` and an optional machine-readable `code` lifted
from the gateway's `{error, code}` body. Branch on `code`, not the message:

```cpp
try {
  auto s = client.sessions.create(o);
} catch (const sb::SolariError& e) {
  if (e.code == sb::error_code::FeatureRequiresPlan)          /* stealth/proxy/captcha needs a paid plan */;
  else if (e.code == sb::error_code::ConcurrencyLimitExceeded) /* at the session cap — back off */;
  else if (e.code == sb::error_code::PlanLimitExceeded)        /* a plan quota is exhausted */;
  else if (e.code == sb::error_code::BrowserUnhealthy)         /* retry: the slot failed its probe */;
  std::cerr << e.what() << " (status " << e.status.value_or(0) << ")\n";
}
```

`status` is unset for transport-level failures (DNS, connect, retries exhausted on transport errors).

## Testing

`tests/test_main.cpp` runs fully offline — no gateway, no network. Every wire
shape is built or parsed by a pure function that the tests call directly:
`buildCreateBody`, `parseSessionResponse`, `parseReplayUrl`, `parseProfile(s)`,
`parseSaveResult`, `parseProxyCountries`, `parseErrorCode`, `throwHttpError`,
`deriveCdpFromWs`, and `HttpTransport::prepare`.

Coverage includes the create-body's omitted-key rules (falsy flags, empty
profileId, both proxy union arms), cdpEndpoint derivation (including
query-preservation and not mangling a host containing "ws"), the storageState
absent-vs-empty tri-state, replay-url defaults (`0` / `"gzip"`), profile parsing
with unknown columns surviving on `raw`, error-code parsing (non-JSON, non-object,
non-string code), the retry predicate (502/503/504 only), and the documented
defaults.

```sh
ctest --test-dir build --output-on-failure   # 40 cases / 156 assertions
```
