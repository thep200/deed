# SETUP — macOS (nhanh & sạch, dùng mise)

Bộ file cài đặt cho dự án **API Client (REST + gRPC)**. Nguyên tắc "sạch": tách 3 lớp công cụ cho 3 trình quản lý khác nhau, **không** `brew install grpc/protobuf` (nguồn rác & xung đột lớn nhất).

| Lớp | Quản lý bởi | Gồm |
|---|---|---|
| 0 — Toolchain Apple | `xcode-select` / App Store | clang, macOS SDK, codesign, notarytool |
| 1 — Host build tools | **mise** (`mise.toml`) | cmake, ninja (+ ccache tuy chon) |
| 2 — Thư viện C/C++ | **vcpkg** (`vcpkg.json`) | cpr, grpc, nlohmann-json (+ openssl/abseil/protobuf… build từ nguồn) |

Vì sao sạch: mise không đụng `/opt/homebrew`; vcpkg manifest giữ mọi lib trong `vcpkg_installed/` của repo (xóa repo là sạch); env build gói gọn trong `mise.toml`.

---

## File trong bộ này

```
api-client-setup/
├── mise.toml                     # lop 1: tools + env (VCPKG_ROOT, Ninja, triplet)
├── vcpkg.json                    # lop 2: khai bao thu vien (manifest mode)
├── CMakeLists.txt                # root skeleton + smoke target (theo README muc 3)
├── CMakePresets.json             # noi vcpkg toolchain + Ninja (commit vao VCS)
├── CMakeUserPresets.json.example # mau cho ai khong dung mise (set VCPKG_ROOT tay)
├── .gitignore
├── tools/smoke/                  # smoke test: verify ca 3 lib link duoc
│   ├── CMakeLists.txt
│   └── main.cpp
└── scripts/
    ├── package_macos.sh          # codesign + dmg + notarytool + staple
    └── entitlements.plist        # hardened-runtime entitlements (network client)
```

---

## Phần A — Cài đặt (lần đầu)

### 1. Toolchain Apple (lớp 0)
```bash
xcode-select --install        # du cho phat trien Core/CLI
# Phase 2 (UI AppKit) + Phase 3 (notarize): cai Xcode day du tu App Store.
```

### 2. mise — host build tools (lớp 1)
Nếu chưa có mise: `curl https://mise.run | sh` rồi kích hoạt theo hướng dẫn shell của bạn.
Trong gốc repo (đã có `mise.toml`):
```bash
mise trust       # tin config local
mise install     # keo cmake/ninja theo version da pin + ap [env]
```
Kiểm tra env đã vào: `mise env | grep VCPKG_ROOT` (phải thấy `~/vcpkg`).

> Apple Silicon dùng `arm64-osx` (đã set sẵn). Nếu máy Intel: sửa `VCPKG_DEFAULT_TRIPLET = "x64-osx"` trong `mise.toml`.

### 3. vcpkg — thư viện C/C++ (lớp 2)
```bash
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
```
`VCPKG_ROOT` đã trỏ sẵn qua `mise.toml` — không cần sửa `~/.zshrc`.

(Khuyến nghị) chốt baseline để build tái lập — ghi `builtin-baseline` vào `vcpkg.json`:
```bash
"$VCPKG_ROOT/vcpkg" x-update-baseline --add-initial-baseline
```

---

## Phần B — Build & verify

```bash
cmake --preset default        # LAN DAU: vcpkg build het deps tu nguon (xem canh bao duoi)
cmake --build build
./build/tools/smoke/smoke     # in version 3 lib -> "Setup OK" la xong
```

### Cảnh báo build lần đầu
- **gRPC rất nặng**: kéo openssl + abseil + protobuf + re2 + c-ares + zlib build từ nguồn → thường **15–40 phút** lần đầu. Sau đó **vcpkg binary cache** (`~/.cache/vcpkg/archives`) làm các lần sau nhanh hẳn; bật thêm **ccache** (tùy chọn, xem `mise.toml`) để tăng tốc biên dịch code của bạn.
- Đẩy song song: `export VCPKG_MAX_CONCURRENCY=$(sysctl -n hw.ncpu)` (hoặc bỏ comment dòng trong `mise.toml`).
- `protoc` dùng **bản vcpkg build ra** (khớp version protobuf) — đừng cài protoc qua brew/mise để tránh lệch version. Dự án đi đường gRPC động/reflection nên protoc chủ yếu cho test.

---

## Phần C — Đóng gói macOS: codesign + notarize (Phase 3)

Mục tiêu: `.app` → `.dmg` đã **ký Developer ID** + **notarize** + **staple** để qua Gatekeeper.

### C.1. Một lần: chứng chỉ & notary profile
1. Có tài khoản **Apple Developer** (Developer ID). Tạo/khởi tạo chứng chỉ **"Developer ID Application"** và import vào Keychain (qua Xcode ▸ Settings ▸ Accounts, hoặc tải từ developer.apple.com).
   - Lấy tên định danh đầy đủ: `security find-identity -v -p codesigning` → dạng `Developer ID Application: Ten Ban (TEAMID)`.
2. Tạo **app-specific password** tại appleid.apple.com (mục Sign-In and Security).
3. **Lưu credential vào Keychain** (làm 1 lần) để script không cần đụng mật khẩu:
   ```bash
   xcrun notarytool store-credentials "api-client-notary" \
     --apple-id "you@example.com" \
     --team-id "TEAMID" \
     --password "app-specific-password"
   ```
   `api-client-notary` là tên profile bạn tự đặt — truyền lại cho script qua `NOTARY_PROFILE`.

> Bảo mật: tự bạn chạy các lệnh trên trong máy mình. Script đóng gói **không** chứa mật khẩu; nó chỉ tham chiếu tên profile đã lưu trong Keychain.

### C.2. Build bản release rồi đóng gói
```bash
cmake --preset release
cmake --build build

APP_PATH="build/ui/macos_appkit/deed.app" \
DEV_ID_APP="Developer ID Application: Ten Ban (TEAMID)" \
NOTARY_PROFILE="api-client-notary" \
./scripts/package_macos.sh
# -> dist/deed.dmg (da ky, notarize, staple)
```

Script làm 5 bước: ký inner→outer với `--options runtime --timestamp` (+ `entitlements.plist` nếu có) → tạo `.dmg` kèm symlink `/Applications` → ký dmg → `notarytool submit --wait` → `stapler staple` + verify.

### C.3. Khi notarize báo "Invalid"
Xem log chi tiết để biết binary nào thiếu chữ ký / thiếu hardened runtime:
```bash
xcrun notarytool log <submission-id> --keychain-profile "api-client-notary"
```
Lỗi hay gặp: dylib/framework nhúng chưa ký, thiếu `--options runtime`, thiếu secure timestamp (`--timestamp`), hoặc dùng SDK quá cũ.

---

## Lối tắt bằng Makefile

Mọi bước trên đã gói vào `Makefile` (gọi tool qua `mise exec --` nên đúng version pin + có `VCPKG_ROOT` kể cả chưa activate mise). `make` không tham số sẽ in danh sách target.

```
make doctor     # kiem tra xcode/mise/cmake/ninja/vcpkg
make setup      # tools + bootstrap vcpkg + unit test Core (lan dau)
make build      # build ra file .app macOS (target deed)
make test       # build nhanh .app (incremental) + mo app de test UI
make build-all  # build toan bo (core + cli + tests + app)
make core-test  # build + chay unit test Core (ctest)
make release    # configure(Release) + build toan bo
make smoke      # build + chay smoke test toolchain
make package DEV_ID_APP="Developer ID Application: Ten (TEAMID)" NOTARY_PROFILE="api-client-notary"
make clean      # xoa build/
make distclean  # xoa build/ dist/ vcpkg_installed/ .cache/
```

Override: `make build USE_MISE=0` (không qua mise), `make bootstrap VCPKG_ROOT=/path/khac`. Lưu credential notarize một lần: `make notary-store` in đúng lệnh cần chạy.

## Ghi chú vận hành
- Commit vào VCS: `mise.toml`, `vcpkg.json`, `CMakeLists.txt`, `CMakePresets.json`, `scripts/`. **Không** commit `CMakeUserPresets.json`, `build/`, `dist/`, `vcpkg_installed/`, `.cache/`.
- CI (GitHub Actions): cache `~/.cache/vcpkg/archives` + `.cache/ccache` để khỏi build gRPC lại mỗi lần; notarize chỉ chạy ở job macOS có secret Developer ID.
- gRPC link tĩnh làm binary phình to — nếu cần `.app` gọn, cân nhắc link động hoặc tách gRPC thành tính năng tùy chọn (xem README mục 2.5 / 10).
