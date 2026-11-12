# Changelog

## v0.5.4

- **`Profile::sizeBytes`.** The gateway returns each profile's `sizeBytes` as a JSON number, but `Profile` did not declare it, so it was reachable only through `raw` (retest D-25). It is now `std::optional<long long> sizeBytes`, empty for a profile that has not been saved yet, as `Profile.sizeBytes` in TypeScript (and `SizeBytes` in Go). `raw` still carries the whole row.
- **The `ProxyOptions::session` doc said "alnum + dash, <=32 chars".** The gateway accepts any label of 1 to 128 characters and turns it into a stable id for the proxy vendor; the doc now says so.

## v0.5.3

- **The gateway's error body no longer carries the session id into error messages.** `GET /sessions/<id>/replay-url` answered `404 ReplayPending` with the full `sessionId` in its body and `throwHttpError` appended that body. The id is the credential inside the ws/cdp URLs. The body is now redacted (`redactBody()`: the request path's id, in any spelling, and every `"sessionId"` field cut to 8 characters); `code` is still parsed from the raw body.

## v0.5.2

- **Errors no longer print the session's credential URLs.** A failed `CdpConnection::connect()` used to put the full CDP endpoint (which carries the session's credential in its path) in the exception text, and a failed signed-URL fetch printed the signed URL. They now show `scheme://host/…` only, via the new `redactUrl()` / `scrubUrl()` helpers. The session id in `/sessions/<id>` (the credential inside those URLs) is cut to its first 8 characters in REST error messages too (`redactSessionPath()`).

## v0.5.1

- `launch`: the default health-probe timeout is now 15 s (was 2 s). On a cold pool slot the first CDP round trip can take longer than 2 s, so a healthy browser could fail the probe and be retried or released. `probeTimeoutMs` still sets it. Go (`ProbeTimeoutMs`) and Rust (`probe_timeout_ms`) use the same 15 s bound for the connect step.

## v0.5.0

- `client.profiles.edit(id, reason)` opens the profile editor on an existing profile and returns a
  `ProfileEditor` with `url`, `expiresAt`, `status()`, `saveAndClose()`, `exit()` and `waitForSave()`.
- `Profile::version` is now populated from `client.profiles.list()`.
