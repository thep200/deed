#!/usr/bin/env bash
#
# package_macos.sh — Codesign + DMG + Notarize (notarytool) + Staple for a Developer ID app.
#
# REQUIREMENTS:
#   - Full Xcode (provides: codesign, hdiutil, xcrun notarytool, xcrun stapler).
#   - A "Developer ID Application" certificate imported into the Keychain.
#   - A notary profile already stored in the Keychain (see SETUP.md, Packaging section).
#   - A pre-built .app (e.g.: cmake --preset release && ... -> build/ui/macos_appkit/deed.app).
#
# HOW TO RUN (pass via environment variables, do NOT hardcode secrets into the file):
#   APP_PATH="build/ui/macos_appkit/deed.app" \
#   DEV_ID_APP="Developer ID Application: Your Name (TEAMID)" \
#   NOTARY_PROFILE="api-client-notary" \
#   ./scripts/package_macos.sh
#
set -euo pipefail

# ---- Config ----
: "${APP_PATH:?Need APP_PATH= path to the built .app (e.g. build/ui/macos_appkit/deed.app)}"
: "${DEV_ID_APP:?Need DEV_ID_APP='Developer ID Application: Your Name (TEAMID)'}"
: "${NOTARY_PROFILE:?Need NOTARY_PROFILE=keychain profile name (notarytool store-credentials)}"

APP_NAME="$(basename "${APP_PATH%.app}")"
DIST_DIR="${DIST_DIR:-dist}"
ENTITLEMENTS="${ENTITLEMENTS:-scripts/entitlements.plist}"
DMG_PATH="${DIST_DIR}/${APP_NAME}.dmg"

if [[ ! -d "$APP_PATH" ]]; then
  echo "Error: .app not found at '$APP_PATH'. Build it first." >&2
  exit 1
fi
mkdir -p "$DIST_DIR"

echo "==> 1/5  Codesign (hardened runtime + secure timestamp)"
# Sign inside-out: the dylibs/frameworks embedded inside the .app first, then the .app.
if [[ -d "$APP_PATH/Contents/Frameworks" ]]; then
  while IFS= read -r -d '' f; do
    codesign --force --options runtime --timestamp --sign "$DEV_ID_APP" "$f"
  done < <(find "$APP_PATH/Contents/Frameworks" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
fi

SIGN_ARGS=(--force --options runtime --timestamp --sign "$DEV_ID_APP")
if [[ -f "$ENTITLEMENTS" ]]; then
  SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
  echo "    (using entitlements: $ENTITLEMENTS)"
fi
codesign "${SIGN_ARGS[@]}" "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

echo "==> 2/5  Create DMG (with /Applications symlink)"
STAGING="$(mktemp -d)"
cp -R "$APP_PATH" "$STAGING/"
ln -s /Applications "$STAGING/Applications"
rm -f "$DMG_PATH"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGING" -ov -format UDZO "$DMG_PATH"
rm -rf "$STAGING"

echo "==> 3/5  Sign DMG"
codesign --force --timestamp --sign "$DEV_ID_APP" "$DMG_PATH"

echo "==> 4/5  Notarize (submit + wait for result)"
# --wait: blocks until Apple returns a result. If Invalid, view the log with:
#   xcrun notarytool log <submission-id> --keychain-profile "$NOTARY_PROFILE"
xcrun notarytool submit "$DMG_PATH" --keychain-profile "$NOTARY_PROFILE" --wait

echo "==> 5/5  Staple ticket into DMG + verify"
xcrun stapler staple "$DMG_PATH"
xcrun stapler validate "$DMG_PATH"
spctl --assess --type open --context context:primary-signature -v "$DMG_PATH" || \
  echo "    (spctl assess: check manually if it warns)"

echo
echo "DONE -> $DMG_PATH (signed, notarized, stapled — passes Gatekeeper)"
