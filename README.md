<h1 align="center">Deed</h1>

![Deed main](images/main.png)

<h1 align="center">Deed is MacOS native API client</h1>

Supported type of request:

- REST/HTTP
- gRPC (unary)

# Features

- Light weight
- Theme retro MacOS
- REST / HTTP
- gRPC (unary)
- Collection tree (lazy load)
- Request editor (JSON)
- Response viewer (JSON)
- Environment & Alias
- Import curl

# Techstack

- C++17 (cross-platform)
- Objective-C++ / AppKit
- Scintilla
- libcurl/cpr
- gRPC C++ + Protobuf
- nlohmann/json
- CMake + Ninja

# Upcomming features

- Search & filter
- gRPC streaming

# Releases

The asset is `deed-<version>-macos-arm64.zip`.

The app is **ad-hoc signed** (not notarized with a Developer ID), so on first launch
macOS Gatekeeper blocks it. To open it:

- Right-click the app → **Open** → **Open**, or
- Remove the quarantine flag: `xattr -dr com.apple.quarantine /Applications/deed.app`
