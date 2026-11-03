# Changelog

## v0.5.0

- `client.profiles.edit(id, reason)` opens the profile editor on an existing profile and returns a
  `ProfileEditor` with `url`, `expiresAt`, `status()`, `saveAndClose()`, `exit()` and `waitForSave()`.
- `Profile::version` is now populated from `client.profiles.list()`.
