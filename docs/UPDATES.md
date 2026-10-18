# Signed updates

Code: `src/update/Update.{hpp,cpp}` (library `ou_update`, no Qt), `tools/ou-sign`, tests in `tests/test_update.cpp`.
Status labels: **observed** = I ran it and saw the result; **inferred** = follows from the code but not exercised; **unknown**.

The design follows the ideas of IKV's updater, as described in `docs/EXISTING_PROJECT_AUDIT.md`: a detached signature over the exact manifest bytes, checked before parsing; an embedded trust key; anti-rollback; a staged, journaled swap. IKV has no licence, so this is a fresh implementation; no IKV code was copied. Differences: libsodium instead of OpenSSL, and directory exchange (`renameat2(RENAME_EXCHANGE)`) instead of symlinks.

## What gets updated

Only OpenUnderworld's own files, in a user-local install root: `$XDG_DATA_HOME/openunderworld/install` (default `~/.local/share/openunderworld/install`). The launcher prefers `install/current/bin/ou-viewer` when it exists. A package-manager install in `/usr` is never touched.

Never game files:
- Manifest paths that look like game data are rejected: `.drm`, `.exe`, `.dll`, `.mul`, `.fsb`, `.big`, three-digit extensions such as `bigfile.000`, and names starting with `bigfile` or `patch.`.
- The install root may not overlap the configured game directory, in either direction.
- A non-empty root without the `.openunderworld-install` marker is refused, so the updater never takes over a foreign directory.

All of these are covered by the `OnlyProjectFilesAndOwnedRoots` test (observed).

## Manifest

`manifest.json` is served next to `manifest.json.sig`, which holds 64 raw bytes: an Ed25519 detached signature over the exact bytes of `manifest.json`.

```json
{
  "version": "0.2.0",
  "channel": "stable",
  "min_version": "0.1.0",
  "changelog": "text shown in the launcher",
  "files": [
    {"path": "bin/ou-viewer", "size": 123, "sha256": "<64 hex>", "url": "https://…/bin/ou-viewer", "executable": true}
  ]
}
```

- `version` / `min_version`: dotted numbers, at most 4 components.
- `min_version`: the oldest installed version allowed to update with this manifest. Older installs are told to reinstall.
- `path`: relative and `/`-separated. Empty components, `.`, `..`, backslashes and absolute paths are rejected, as are the reserved names `.ou-manifest.json[.sig]` and duplicates.
- Every `url` must be `https://`.
- Size limit: 1 MiB. The manifest is parsed only after the signature verifies (`verifyAndParse`).

## Client flow (`ou::update::update`)

1. **Disable switch.** If config `updates.enabled` is false, or `OU_DISABLE_UPDATES` is set to anything other than `0`, return `Disabled` without touching the network.
2. **Fetch.** Download `manifest.json` (≤1 MiB) and `manifest.json.sig` (≤64 bytes) from the configured HTTPS URL. The transport is libcurl:
   - `CURLOPT_PROTOCOLS_STR`/`REDIR_PROTOCOLS_STR = "https"`, so redirects are https-only too
   - TLS ≥ 1.2 with the system CA store
   - at most 5 redirects
   - a byte cap per transfer
   - low-speed abort
3. **Verify, then parse.** Check the signature against the **compiled-in** public key (`crypto_sign_verify_detached`), then validate the JSON.
4. **Channel and anti-rollback.**
   - The manifest channel must equal the configured channel.
   - An offered version older than the installed one is an error ("rollback refused"); an equal one means up to date.
   - `min_version` is enforced.
   - `install()` re-checks this under the lock.
5. **Stage.**
   - Take `flock(install/.lock)` and run recovery.
   - Download each file with a cap equal to its declared size, check the exact size and SHA-256, and write it into `install/.staging/` with fsync and mode 0755/0644.
   - Store the signed manifest and signature as `.staging/.ou-manifest.json[.sig]`.
   - On any failure, `.staging` is deleted and `current` is untouched.
6. **Journal.** Write `install/.journal` = `{"version": "<new>"}` with fsync, plus a directory fsync. From here on, the staged copy is known complete.
7. **Swap** (`finishSwap`, idempotent):
   - move the old `previous` to `.trash` and delete it
   - `renameat2(.staging, current, RENAME_EXCHANGE)`
   - rename `.staging` (now the old version) to `previous`
   - delete the journal

## Recovery

`recover(root)` runs at every launcher start and at the beginning of every install or rollback:

| State found | Action |
|---|---|
| `.journal` and `current` ≠ target, `.staging` = target | roll forward (finish the swap) |
| `.journal` and `current` = target | finish the swap: leftover `.staging` → `previous`; drop the journal |
| `.journal` but `.staging` incomplete/missing | abandon: delete `.staging` and the journal; keep `current` |
| unreadable `.journal` | delete `.staging` and the journal |
| `.staging` without `.journal` (interrupted download) | delete `.staging` |
| `.trash` | delete |

Every crash point has a test in `InterruptedSwapRecovery` (observed): before anything moved, after the exchange, after the old backup moved to `.trash`, incomplete staging, and download interrupted. That test builds the on-disk states directly; no real power cut was simulated.

## Rollback

`rollback(root)` exchanges `previous` and `current`, which is the launcher's "Roll back to previous" button. This is a deliberate user action, so it is allowed even though it activates an older version. The next update check will offer the newer release again. There is no persistent per-channel highwater: anti-rollback compares against the installed version only.

## Tests

`ou_update_tests`: 12 tests, all observed passing under ASan/UBSan on 2026-10-18 (`build-launcher/ou_update_tests`).

- No network. A test-only transport maps `https://updates.test/<rel>` to `file://<tmp>/srv/<rel>`, so the `https://` rules stay in force.
- Coverage: good signature; bad signature (wrong key, truncated signature, no payload fetched); tampered manifest (field edit, bit flip); size too small and too large; hash mismatch; rollback attempt (older, signed, via both `update` and `install`); `min_version`; user rollback; interrupted-swap recovery; disable switch (config and environment, transport never called); unsafe paths, game-data paths, non-https URLs, duplicates, foreign root, game-dir overlap.
- `HttpsTransportRefusesOtherSchemes` checks that the libcurl transport rejects `http://`, `file://` and `ftp://` before connecting.
- The real HTTPS path against a live server has **not** been exercised: no release server exists yet (unknown).

## Release key ceremony

The release key pair was generated on 2026-10-18 with `ou-sign keygen` on the maintainer workstation (observed):
- Secret key: `~/.config/openunderworld-signing/release.key`, 64 bytes, mode 0600, directory 0700.
- Public key: committed as `packaging/release-pubkey.hex` (`1bac0f21…ab7303`) and compiled in through CMake (`OU_UPDATE_PUBLIC_KEY`, read from that file by default).

`ou-sign keygen` refuses to write into any directory inside a git work tree and never overwrites an existing key. `.gitignore` also ignores `*.key`.

Procedure for each release:

1. Build the release binaries from a clean, tagged tree. Lay out the update payload, e.g. `payload/bin/ou-viewer`, `payload/bin/ou-launcher`.
2. `ou-sign manifest --version X.Y.Z --channel stable --min-version A.B.C --changelog CHANGES.txt --base-url https://<host>/<path>/X.Y.Z payload > manifest.json`. This applies the client's validation, so game-data-like files are refused.
3. `ou-sign sign manifest.json`. It writes `manifest.json.sig`, re-verifies it, and warns if the key differs from the one compiled into this `ou-sign`.
4. `ou-sign verify manifest.json`, which checks against the compiled-in key.
5. Upload `payload/*`, `manifest.json` and `manifest.json.sig`. Set the launcher's manifest URL, or a default in config, to the HTTPS URL of `manifest.json`.

Key hygiene and rotation:
- Back up `release.key` offline, e.g. on an encrypted USB stick. Losing it means users must reinstall to trust a new key.
- **Rotation:**
  1. `ou-sign keygen --dir <new dir>`.
  2. Ship one release signed with the **old** key whose binaries embed the **new** public key.
  3. Sign later releases with the new key.
- **Compromise:** the old key cannot be revoked remotely (there is no revocation list; unknown whether one will be needed). Publish a notice and a manually installed release with a new key.
- CI never holds the secret key; signing is local only.

## Not done / open

- No per-channel persistent highwater against an attacker who downgrades `current` locally. Anyone who can write the install root can already replace the binaries.
- Downloads are buffered in memory (`ponytail` limit: fine for tens of MB; stream to disk if payloads grow).
- No release server or repo exists yet, so `updates.manifest_url` defaults to empty and the launcher says so.
