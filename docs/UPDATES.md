# Signed updates

Code: `src/update/Update.{hpp,cpp}` (library `ou_update`, no Qt), `tools/ou-sign`, tests in `tests/test_update.cpp`.
Status labels: **observed** = I ran it and saw the result; **inferred** = follows from the code but not exercised; **unknown**.

The design follows the ideas of IKV's updater, as described in `docs/EXISTING_PROJECT_AUDIT.md`: a detached signature over the exact manifest bytes, checked before parsing; an embedded trust key; anti-rollback; a staged, journaled swap. IKV has no licence, so this is a fresh implementation; no IKV code was copied. Differences: libsodium instead of OpenSSL, and directory exchange (`renameat2(RENAME_EXCHANGE)`) instead of symlinks.

## What gets updated

Two modes, both limited to OpenUnderworld's own files:

- **AppImage mode** (the launcher runs inside an AppImage, i.e. `$APPIMAGE` is set): the AppImage file itself is updated, see [AppImage self-update](#appimage-self-update). The install root is not used.
- **Install-root mode** (any other launcher): a user-local install root, `$XDG_DATA_HOME/openunderworld/install` (default `~/.local/share/openunderworld/install`). The launcher prefers `install/current/bin/ou-viewer` when it exists. A package-manager install in `/usr` is never touched.

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

## Client flow (`ou::update::update`, install-root mode)

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

## AppImage self-update

`ou::update::Options::appImage` (the launcher sets it from `$APPIMAGE`) plus `runningVersion` (the launcher's compiled `OU_VERSION`) switch `check`/`update` to AppImage mode:

1. Disable switch, fetch, **verify the signature, then parse**, channel check: as above.
2. Anti-rollback compares the manifest against `runningVersion`: older → "rollback refused", equal → up to date, `min_version` enforced.
3. `installAppImage`:
   - Resolves `$APPIMAGE` with `canonical()`, so a symlinked AppImage updates the real file; the link is repointed at the new file (atomically) and stays the path to run.
   - Takes `flock(LOCK_EX|LOCK_NB)` on the AppImage file itself, so no lock file appears next to it.
   - Picks the manifest's single top-level `*.AppImage` entry; if there is none, it errors.
   - Downloads it with a cap equal to the declared size, then checks the exact size and SHA-256.
   - Writes `OpenUnderworld-<version>-x86_64.AppImage.new` in the same directory (same filesystem) with fsync and mode 0755. A stale `.new` from an interrupted download is deleted first.
   - Deletes older `OpenUnderworld*.AppImage.bak` siblings (only one backup is kept), then hard-links the running file to `<old>.bak`; filesystems without hard links get a copy.
   - `rename(.new, OpenUnderworld-<version>-x86_64.AppImage)`: atomic. Until the next step both names run.
   - Rewrites every `.desktop` in `Options::applicationsDir` (the launcher passes `$XDG_DATA_HOME/applications`) whose `Exec=`/`TryExec=` runs the old path: both lines point at the new file, `X-AppImage-Version=` (if present) becomes the new version, and an entry named after the old file stem (e.g. `openunderworld-0.10.0-x86_64.desktop`, case style kept) is renamed. Written via a temp file + rename. `Icon=` and other entries are left alone.
   - Deletes the old-named file. The running process keeps its inode, still reachable as `<old>.bak`. Directories are fsynced.
   - Returns the path to run from now on (`Result::appImage`). The launcher sets `$APPIMAGE` to it; its **Restart** button starts it and quits; the next rollback starts from it.

   On a size/hash mismatch nothing is renamed and `.new` is removed. A crash leaves at least one runnable name, and the desktop entry still points at a file until the old name is deleted.
4. `rollbackAppImage` restores `<AppImage>.bak`, or else the single `OpenUnderworld*.AppImage.bak` sibling, under its old name. The current file becomes `<current>.bak`, the desktop entries point back (X-AppImage-Version = the version in the restored `OpenUnderworld-<v>-x86_64.AppImage` name, unchanged for other names), and the restored path is returned. A backup with the current name is exchanged with `renameat2(RENAME_EXCHANGE)`. It is the launcher's "Roll back to previous" in AppImage mode; pressing it again re-applies the newer one.

Headless: `OpenUnderworld.AppImage --check-update` / `--update` print one line (with the new path) and exit (0 = success). Without a display, set `QT_QPA_PLATFORM=offscreen`.

The release manifest (`tools/release.sh`) lists only the AppImage:

```json
{"version": "0.1.0", "channel": "stable", "min_version": "", "changelog": "…",
 "files": [{"path": "OpenUnderworld-0.1.0-x86_64.AppImage", "size": …, "sha256": "…",
            "url": "https://github.com/sergen213/openunderworld/releases/download/v0.1.0/OpenUnderworld-0.1.0-x86_64.AppImage",
            "executable": true}]}
```

An install-root launcher (e.g. the Arch package) that applies this manifest stores the AppImage under `install/current/`. That is harmless but useless, because the launcher looks for `install/current/bin/ou-viewer` [I]. Package installs are expected to update through the package manager.

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

`ou_update_tests`: 15 updater tests (+ 5 Config) (14 under ASan/UBSan on 2026-10-18 [O]; the AppImage ones rewritten 2026-10-30 [O]).

- No network. A test-only transport maps `https://updates.test/<rel>` to `file://<tmp>/srv/<rel>`, so the `https://` rules stay in force.
- Coverage: good signature; bad signature (wrong key, truncated signature, no payload fetched); tampered manifest (field edit, bit flip); size too small and too large; hash mismatch; rollback attempt (older, signed, via both `update` and `install`); `min_version`; user rollback; interrupted-swap recovery; disable switch (config and environment, transport never called); unsafe paths, game-data paths, non-https URLs, duplicates, foreign root, game-dir overlap.
- `AppImageUpdateTest` (a `bin/` + `share/applications/` layout like an integrator leaves it):
  - `RefusalsLeaveTheAppImageAlone`: up to date and older refused against `runningVersion`; hash mismatch and a manifest without an AppImage leave the file, desktop entry and directory untouched;
  - `UpdateRenamesAndRetargetsDesktopEntry`: new name, executable, `<old>.bak`, stale `.new` and older OpenUnderworld `.bak` removed, unrelated files kept; desktop entry rewritten (Exec with `%U`, TryExec, version) and renamed, entries for other paths or for `.bak` untouched; a second update keeps one backup;
  - `RollbackRestoresOldNameAndDesktopEntry`: rollback from the returned path restores the old name and the exact original desktop entry; again re-applies the newer one;
  - `CustomNameAndSymlink`: a user-named file is renamed and rolled back to its name; a symlinked `$APPIMAGE` is repointed and stays the returned path.
- `HttpsTransportRefusesOtherSchemes` checks that the libcurl transport rejects `http://`, `file://` and `ftp://` before connecting.
- The live HTTPS path against GitHub releases: see the v0.1.0 verification below.

### Live verification against the v0.1.0 GitHub release (2026-10-18)

- [O] `curl -I …/releases/latest/download/manifest.json` → 302 to `…/releases/download/v0.1.0/manifest.json` → 302 to `release-assets.githubusercontent.com` (https all the way, within the 5-redirect cap).
- [O] The published AppImage, run with the default config (`env -i`, fresh `HOME`): `--check-update` and `--update` both print `Up to date (0.1.0).` The signature was verified over real HTTPS with libcurl.
- [O] End-to-end self-update from an older AppImage. Built from the v0.1.0 tree with only `project(VERSION)` changed to 0.0.9, in a throwaway worktree, never committed:
  - copied to `<tmp>/apps/OpenUnderworld.AppImage`; `--version` printed `ou-launcher 0.0.9`;
  - `--check-update` printed `Signed update 0.1.0 available.`;
  - `--update`, running from the FUSE mount, so it replaced its own running file: it downloaded the 90 098 168-byte AppImage and printed `Installed 0.1.0 into …/OpenUnderworld.AppImage (old one kept as .bak)`;
  - afterwards `OpenUnderworld.AppImage --version` printed `0.1.0` and `OpenUnderworld.AppImage.bak --version` printed `0.0.9`. The SHA-256 of the replaced file equals the published `.sha256` (`3c76f545…a99a133`), and a second `--update` reports up to date.
- [O] `ou-sign verify manifest.json` on the downloaded release assets: `OK: version 0.1.0, channel stable, 1 files`.
- [O] 2026-10-30, the user's layout in a temp `HOME` (never the real one): `~/.local/bin/OpenUnderworld-0.10.0-x86_64.AppImage` (a 0.11.0 build of this tree with a throwaway test key compiled in) plus a stale `…0.10.0-x86_64.AppImage.bak`, and a copy of the integrator-made `openunderworld-0.10.0-x86_64.desktop`. The manifest for a 0.12.0 build was made with `ou-sign manifest` and signed with the test key, served over local HTTPS (self-signed cert trusted only inside `unshare -Urm` via a bind-mounted CA bundle). Results:
  - `--update` (run via the desktop `Exec`) installed `OpenUnderworld-0.12.0-x86_64.AppImage`, left `OpenUnderworld-0.10.0-x86_64.AppImage.bak` (the running 0.11.0, sha256 equal), removed the old name and renamed the entry to `openunderworld-0.12.0-x86_64.desktop` with Exec/TryExec/X-AppImage-Version 0.12.0; the new `Exec` printed `ou-launcher 0.12.0` and up to date.
  - GUI on Xvfb (xdotool clicks, screenshots viewed): "Roll back to previous" restored the 0.10.0 name and desktop entry; **Restart** came up as 0.11.0 with `$APPIMAGE` = the restored path; "Download & install" then **Restart** came up as 0.12.0 from the new name.

## Release key ceremony

The release key pair was generated on 2026-10-18 with `ou-sign keygen` on the maintainer workstation (observed):
- Secret key: `~/.config/openunderworld-signing/release.key`, 64 bytes, mode 0600, directory 0700.
- Public key: committed as `packaging/release-pubkey.hex` (`1bac0f21…ab7303`) and compiled in through CMake (`OU_UPDATE_PUBLIC_KEY`, read from that file by default).

`ou-sign keygen` refuses to write into any directory inside a git work tree and never overwrites an existing key. `.gitignore` also ignores `*.key`.

Procedure for each release: `tools/release.sh` (docs/RELEASING.md) does all of it:

1. Build the AppImage from a clean, tagged worktree. The payload is the AppImage only.
2. `ou-sign manifest --version X.Y.Z --channel stable --changelog <CHANGELOG section> --base-url https://github.com/sergen213/openunderworld/releases/download/vX.Y.Z/ payload > manifest.json`. This applies the client's validation, so game-data-like files are refused.
3. `ou-sign sign manifest.json`. It writes `manifest.json.sig`, re-verifies it, and warns if the key differs from the one compiled into this `ou-sign`; release.sh treats that warning as fatal.
4. `ou-sign verify manifest.json`, which checks against the compiled-in key.
5. Upload the AppImage, `manifest.json` and `manifest.json.sig` as GitHub release assets.

The launcher's default `updates.manifest_url` is `https://github.com/sergen213/openunderworld/releases/latest/download/manifest.json`; the signature is fetched from the same URL plus `.sig`. GitHub redirects `latest/download` to the newest non-pre-release, so releases are published as normal releases marked experimental.

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
- Downloads are buffered in memory (`ponytail` limit: an ~86 MB AppImage is fine; stream to disk if it grows much larger).
- The AppImage update is not resumable; an interrupted download restarts.
