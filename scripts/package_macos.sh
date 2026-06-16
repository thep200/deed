#!/usr/bin/env bash
#
# package_macos.sh — Codesign + DMG + Notarize (notarytool) + Staple cho app Developer ID.
#
# YEU CAU:
#   - Xcode day du (cung cap: codesign, hdiutil, xcrun notarytool, xcrun stapler).
#   - Chung chi "Developer ID Application" da import vao Keychain.
#   - Mot notary profile da luu san trong Keychain (xem SETUP.md, muc Packaging).
#   - Da build san file .app (vd: cmake --preset release && ... -> build/ApiClient.app).
#
# CACH CHAY (truyen qua bien moi truong, KHONG hardcode secret vao file):
#   APP_PATH="build/ApiClient.app" \
#   DEV_ID_APP="Developer ID Application: Ten Ban (TEAMID)" \
#   NOTARY_PROFILE="api-client-notary" \
#   ./scripts/package_macos.sh
#
set -euo pipefail

# ---- Cau hinh ----
: "${APP_PATH:?Can APP_PATH= duong dan toi .app da build (vd build/ApiClient.app)}"
: "${DEV_ID_APP:?Can DEV_ID_APP='Developer ID Application: Ten Ban (TEAMID)'}"
: "${NOTARY_PROFILE:?Can NOTARY_PROFILE=ten keychain profile (notarytool store-credentials)}"

APP_NAME="$(basename "${APP_PATH%.app}")"
DIST_DIR="${DIST_DIR:-dist}"
ENTITLEMENTS="${ENTITLEMENTS:-scripts/entitlements.plist}"
DMG_PATH="${DIST_DIR}/${APP_NAME}.dmg"

if [[ ! -d "$APP_PATH" ]]; then
  echo "Loi: khong thay .app tai '$APP_PATH'. Build truoc da." >&2
  exit 1
fi
mkdir -p "$DIST_DIR"

echo "==> 1/5  Codesign (hardened runtime + secure timestamp)"
# Ky tu trong ra ngoai: cac dylib/framework nhung ben trong .app truoc, roi den .app.
if [[ -d "$APP_PATH/Contents/Frameworks" ]]; then
  while IFS= read -r -d '' f; do
    codesign --force --options runtime --timestamp --sign "$DEV_ID_APP" "$f"
  done < <(find "$APP_PATH/Contents/Frameworks" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
fi

SIGN_ARGS=(--force --options runtime --timestamp --sign "$DEV_ID_APP")
if [[ -f "$ENTITLEMENTS" ]]; then
  SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
  echo "    (dung entitlements: $ENTITLEMENTS)"
fi
codesign "${SIGN_ARGS[@]}" "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

echo "==> 2/5  Tao DMG (kem symlink /Applications)"
STAGING="$(mktemp -d)"
cp -R "$APP_PATH" "$STAGING/"
ln -s /Applications "$STAGING/Applications"
rm -f "$DMG_PATH"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGING" -ov -format UDZO "$DMG_PATH"
rm -rf "$STAGING"

echo "==> 3/5  Ky DMG"
codesign --force --timestamp --sign "$DEV_ID_APP" "$DMG_PATH"

echo "==> 4/5  Notarize (submit + cho ket qua)"
# --wait: dung den khi Apple tra ket qua. Neu Invalid, xem log bang:
#   xcrun notarytool log <submission-id> --keychain-profile "$NOTARY_PROFILE"
xcrun notarytool submit "$DMG_PATH" --keychain-profile "$NOTARY_PROFILE" --wait

echo "==> 5/5  Staple ticket vao DMG + verify"
xcrun stapler staple "$DMG_PATH"
xcrun stapler validate "$DMG_PATH"
spctl --assess --type open --context context:primary-signature -v "$DMG_PATH" || \
  echo "    (spctl assess: kiem tra thu cong neu canh bao)"

echo
echo "XONG -> $DMG_PATH (da ky, da notarize, da staple — qua Gatekeeper)"
