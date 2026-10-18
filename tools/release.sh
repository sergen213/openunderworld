#!/usr/bin/env bash
# One-command signed release of the pushed HEAD of main: tools/release.sh [--dry-run]
#   1. checks: signing key, HEAD == origin/main, tag/release absent, CHANGELOG section present
#   2. clean detached worktree of HEAD (uncommitted work in your tree is never released)
#   3. full ctest (Debug + ASan/UBSan; real-data tests run when OU_GAME_DIR is set)
#   4. tag vX.Y.Z, build the AppImage, smoke it, ou-sign manifest + sign + verify
#   5. push the tag, gh release create, download the assets back and verify them
# --dry-run stops after step 4 and deletes the local tag. The secret key never leaves
# ~/.config/openunderworld-signing. Bump project(VERSION) + CHANGELOG.md first. Notes: docs/RELEASING.md.
set -euo pipefail

REPO=sergen213/openunderworld
SRC="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"
KEY_DIR="${OU_SIGNING_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/openunderworld-signing}"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1
die() { echo "release: $*" >&2; exit 1; }
say() { echo "== $*"; }
# Heavy steps get a memory cap where systemd user scopes exist (workstation OOM protection).
heavy() {
  if command -v systemd-run >/dev/null && systemd-run --user --scope -q -- true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${OU_RELEASE_MEMMAX:-6G}" -- "$@"
  else "$@"; fi
}

cd "$SRC"
# ---- 1. preconditions --------------------------------------------------------------------------------
[ -f "$KEY_DIR/release.key" ] || die "no signing key at $KEY_DIR/release.key"
[ "$(stat -c %a "$KEY_DIR/release.key")" = 600 ] || die "$KEY_DIR/release.key must be mode 0600"
case "$(realpath "$KEY_DIR")/" in "$(realpath "$SRC")"/*) die "signing key directory is inside the repository" ;; esac
for t in gh cmake ninja ctest curl sha256sum; do command -v "$t" >/dev/null || die "missing $t"; done
git fetch -q origin main
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || die "HEAD is not origin/main: commit, pull --rebase and push first"
VERSION="$(sed -nE 's/^project\(OpenUnderworld VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
TAG="v$VERSION"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG exists; bump project(VERSION) in CMakeLists.txt"
git ls-remote --exit-code --tags origin "$TAG" >/dev/null && die "tag $TAG exists on origin"
gh release view "$TAG" -R "$REPO" >/dev/null 2>&1 && die "release $TAG exists"
CHANGES="$(awk -v v="$VERSION" '$0 ~ "^## \\[" v "\\]" {f=1; next} /^## \[/ {f=0} f' CHANGELOG.md)"
[ -n "$(echo "$CHANGES" | tr -d '[:space:]')" ] || die "CHANGELOG.md has no '## [$VERSION]' section"
if [ -n "$(git status --porcelain)" ]; then
  echo "note: uncommitted changes in $SRC are NOT part of this release (it builds a clean worktree of $(git rev-parse --short HEAD))"
fi

# ---- 2. clean worktree ---------------------------------------------------------------------------------
WORK="$SRC/build-release/$TAG"
TAGGED=0
cleanup() {
  git -C "$SRC" worktree remove --force "$WORK/src" 2>/dev/null || true
  if [ "$TAGGED" = 1 ]; then git -C "$SRC" tag -d "$TAG" >/dev/null; echo "release: removed local tag $TAG"; fi
}
trap cleanup EXIT
rm -rf "$WORK"
mkdir -p "$WORK"
git worktree prune
git worktree add -q --detach "$WORK/src" HEAD
[ -z "$(git -C "$WORK/src" status --porcelain)" ] || die "release worktree is not clean"

# ---- 3. tests --------------------------------------------------------------------------------------------
say "ctest (Debug + ASan/UBSan) on $(git rev-parse --short HEAD)"
[ -n "${OU_GAME_DIR:-}" ] || echo "note: OU_GAME_DIR unset, real-data tests will skip"
heavy cmake -S "$WORK/src" -B "$WORK/test" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOU_SANITIZE=ON >/dev/null
heavy cmake --build "$WORK/test" -j "${OU_JOBS:-12}" >/dev/null
QT_QPA_PLATFORM=offscreen OU_DISABLE_UPDATES=1 heavy ctest --test-dir "$WORK/test" --output-on-failure \
  > "$WORK/ctest.log" 2>&1 || { tail -40 "$WORK/ctest.log"; die "ctest failed (log: $WORK/ctest.log)"; }
grep -E 'tests passed|tests failed' "$WORK/ctest.log"

# ---- 4. tag, AppImage, signed manifest ------------------------------------------------------------------
git tag -a "$TAG" -m "OpenUnderworld $VERSION" HEAD
TAGGED=1
say "AppImage $VERSION"
DIST="$WORK/dist"
OU_APPIMAGE_BUILD_DIR="$WORK/appimage" heavy "$WORK/src/packaging/appimage/build-appimage.sh" "$DIST" > "$WORK/appimage.log" 2>&1 \
  || { tail -40 "$WORK/appimage.log"; die "AppImage build failed (log: $WORK/appimage.log)"; }
APPNAME="OpenUnderworld-$VERSION-x86_64.AppImage"
APP="$DIST/$APPNAME"
[ -f "$APP" ] || die "expected $APP (git describe did not match $TAG?)"
QT_QPA_PLATFORM=offscreen OU_DISABLE_UPDATES=1 APPIMAGE_EXTRACT_AND_RUN=1 "$APP" --self-test || die "AppImage launcher self-test failed"
APPIMAGE_EXTRACT_AND_RUN=1 "$APP" game --help >/dev/null 2>&1 || [ $? -lt 128 ] || die "AppImage 'game' crashed"

OU_SIGN="$WORK/appimage/ou-sign"
mkdir -p "$WORK/payload"
ln "$APP" "$WORK/payload/$APPNAME"
NOTES="$WORK/notes.md"
cat > "$NOTES" <<EOF
**Experimental build.** You need your own legally owned copy of Tomb Raider: Underworld (GOG build, \`tru.exe\` 1.1.0.0). No game data is included.

Run it:
\`\`\`
chmod +x $APPNAME
./$APPNAME                                    # launcher
./$APPNAME game --game-dir "/path/to/Tomb Raider Underworld"
\`\`\`
AppImages from 0.1.0 on update in place: launcher tab **Version & Updates** → *Check for updates* → *Download & install*, or \`./$APPNAME --update\`. The update manifest is signed with the project's Ed25519 key (\`manifest.json.sig\`); \`sha256sum -c $APPNAME.sha256\` checks the download.

Requirements: x86_64 Linux with a current glibc/libstdc++ (built on Arch Linux), Mesa or another OpenGL 4.6 / Vulkan 1.3 driver, FUSE (or run with \`--appimage-extract-and-run\`).

$CHANGES
EOF
echo "$CHANGES" > "$WORK/changelog.txt"
"$OU_SIGN" manifest --version "$VERSION" --channel stable --changelog "$WORK/changelog.txt" \
  --base-url "https://github.com/$REPO/releases/download/$TAG/" "$WORK/payload" > "$DIST/manifest.json"
"$OU_SIGN" sign --dir "$KEY_DIR" "$DIST/manifest.json" 2> "$WORK/sign.log" || { cat "$WORK/sign.log"; die "signing failed"; }
! grep -q WARNING "$WORK/sign.log" || { cat "$WORK/sign.log"; die "signing key is not the release key compiled into this build"; }
"$OU_SIGN" verify "$DIST/manifest.json"
(cd "$DIST" && sha256sum -c "$APPNAME.sha256")
ASSETS=("$APP" "$APP.sha256" "$DIST/manifest.json" "$DIST/manifest.json.sig")
[ -f "$APP.zsync" ] && ASSETS+=("$APP.zsync")

if [ "$DRY" = 1 ]; then
  say "dry run: assets in $DIST, notes $NOTES; local tag will be removed"
  exit 0
fi

# ---- 5. publish + verify the published assets ------------------------------------------------------------
git push -q origin "$TAG"
TAGGED=0
gh release create "$TAG" -R "$REPO" --verify-tag --latest --title "OpenUnderworld $VERSION (experimental)" \
  --notes-file "$NOTES" "${ASSETS[@]}"
CHECK="$(mktemp -d)"
gh release download "$TAG" -R "$REPO" -D "$CHECK"
(cd "$CHECK" && sha256sum -c "$APPNAME.sha256" && "$OU_SIGN" verify manifest.json && cmp manifest.json "$DIST/manifest.json")
rm -rf "$CHECK"
say "published https://github.com/$REPO/releases/tag/$TAG"
