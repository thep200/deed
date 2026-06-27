![Deed main](assets/images/main.png)

<h1 align="center">Deed ✨</h1>
<p align="center"> A lightweight, native macOS API client in retro Mac OS 9 style that speaks REST/HTTP, SSE, gRPC, WebSocket, and GraphQL from a single <=50 MB binary. </p>

![Author](https://img.shields.io/static/v1?label=author&message=Thep200&color=0284c7)
![License](https://img.shields.io/static/v1?label=init-date&message=01-05-2026&color=7e22ce)
![GitHub stars](https://img.shields.io/github/stars/Thep200/deed?style=flat&color=eab308)
![GitHub license](https://img.shields.io/github/license/Thep200/deed?color=16a34a)

- [Overview](#overview)
- [Installation](#installation)
- [Features](#features)
- [Techstack](#techstack)
- [Configuration](#configuration)
  - [Global config](#global-config)
  - [Environments](#environments)
- [Request type](#request-type)
  - [Config](#config)
  - [RESTFUL HTTP](#restful-http)
    - [Request Body](#request-body)
    - [Request Headers](#request-headers)
    - [Request Query](#request-query)
    - [Request Auth](#request-auth)
    - [Response Body](#response-body)
    - [Response Headers](#response-headers)
    - [Response Request](#response-request)
  - [SSE](#sse)
  - [gRPC](#grpc)
  - [Websocket](#websocket)
  - [GraphQL](#graphql)
- [Releases](#releases)

# Overview

Supports:

- RESTFUL / HTTP
- SSE
- gRPC
- WebSocket
- GraphQL

# Installation

Currently, I'm still shipping deed files as `.app`. Please download the latest version from the release section to use it. Features for upgrading the app and shipping `.dmg` files will be implemented soon.

# Features

- Light weight
  - Single binary
  - App size <50MB
  - Runtime memory <150MB for normal usage
- Vintage theme MacOS 9 style
- RESTFUL / HTTP
- SSE
- gRPC
- WebSocket
- GraphQL
- Lazy load collection tree
- Request editor (JSON)
- Response viewer (JSON)
- Environment & Alias
- Import curl, gRPC

# Techstack

- C++17 (cross-platform)
- Objective-C++ / AppKit
- Scintilla
- libcurl/cpr
- gRPC C++ + Protobuf
- nlohmann/json
- CMake + Ninja

# Configuration

## Global config

```json
{
  "font_name": "Monaco 9",
  "font_size": 21,
  "ram_cache_size": 32,
  "disk_cache_size": 256
}
```

Deed caches responses in two tiers, RAM and disk:

* The RAM cache can be set up to **128 MB**.
* The disk cache can be set up to **512 MB**.

`ram_cache_size` and `disk_cache_size` are the levels you choose; anything higher than the limits above is capped automatically.

## Environments

There are no global environments yet — it may be added later. When you import a cURL or gRPC command, Deed automatically creates the matching aliases for you.

Use `{{key}}` anywhere a value is accepted (URL, headers, query, auth, gRPC metadata, …) to insert an environment value. If a key doesn't exist it's left as-is (`{{key}}`) and flagged; if it exists but is empty it becomes an empty string.

An environment file is just a list of keys:

```json
{
  "schemaVersion": 1,
  "keys": [
    {
      "key": "baseUrl",
      "value": "https://api.example.com",
      "enabled": 1
    },
    {
      "key": "token",
      "value": "eyJhbGciOiJI...",
      "enabled": 1
    }
  ]
}
```

> **NOTE:** Secrets are **not** encrypted yet, so double-check your environment files before syncing them to GitHub.

# Request type

Give the app 30 minutes hands-on and this will all click — but here's the reference.

Each request has editing tabs on the **left** and a read-only response on the **right**. Whatever you fill into the tabs is saved to one small JSON file per request, so you can keep an entire collection in a folder and sync it with git.

> Files don't support comments. To turn a single line in **Headers**, **Query**, or **Metadata** on or off, set its `enabled` to `0` instead of deleting it.

## Config

The **Config** tab (the last left-hand tab, on every request type) holds two settings shared by all types:

```json
{
  "timeout_ms": 1800000,
  "tls": true
}
```

- `timeout_ms` — how long to wait before giving up. For HTTP/GraphQL it's the request timeout, for gRPC the deadline, for WebSocket the idle timeout.
- `tls` — verify the server's TLS certificate (for gRPC, connect over a secure channel).

> The **Pretty** button has four modes — **Pretty / Raw / Encode / Decode**. Click into the pane you want it to act on first, then press it.

## RESTFUL HTTP

Paste a `curl` command straight into the URL field and Deed turns it into a new request automatically.

### Request Body

The **Body** tab uses one mode at a time. Pick the mode that matches what you're sending.

No body:

```json
{}
```

JSON:

```json
{
  "name": "deed"
}
```

Form (URL-encoded):

```json
[
  {
    "key": "",
    "value": "",
    "enabled": true
  }
]
```

File - Binary:

```json
{
  "filePath": "/thep200/deed/foo.bin"
}
```

### Request Headers

The **Headers** tab is a list of key/value lines. Duplicate keys are allowed. Set `enabled` to `0` to keep a line but skip sending it.

```json
[
  {
    "key": "Content-Type",
    "value": "application/json",
    "enabled": 1
  },
  {
    "key": "X-Debug",
    "value": "1",
    "enabled": 0
  }
]
```

### Request Query

Deed auto parse and cut query in URL into tab for you.

URL: localhost:8080/api/products?limit=1000

```json
[
  {
    "enabled": 1,
    "key": "limit",
    "value": "1000"
  }
]
```

### Request Auth

The **Auth** tab offers four types: **None**, **Basic**, **Bearer**, and **API key**.

None:

```json
{
  "type": "none"
}
```

Basic — username and password:

```json
{
  "type": "basic",
  "basic": {
    "username": "admin",
    "password": "s3cret"
  }
}
```

Bearer — a token:

```json
{
  "type": "bearer",
  "bearer": {
    "token": "{{token}}"
  }
}
```

API key, sent as a header (`in` = `"header"`):

```json
{
  "type": "apikey",
  "apikey": {
    "key": "X-API-Key",
    "value": "{{apiKey}}",
    "in": "header"
  }
}
```

### Response Body

The **Response** tab (read-only) shows the response body, pretty-printed when it's JSON.

### Response Headers

The right-hand **Headers** tab (read-only) lists the response headers exactly as the server returned them. Any `Set-Cookie` values also appear in the **Cookie** tab.

### Response Request

The right-hand **Request** tab (read-only) shows the exact request that was sent — after variables are substituted, auth is applied, and disabled lines are removed. Handy for checking what the server actually received.

## SSE

Server-Sent Events is simply an HTTP request that streams. You add `Accept: text/event-stream` header into http request and deed do the rest.

![SSE](assets/images/sse.png)

## gRPC

Deed auto reflect schema to get rpc if server enable

![gRPC](assets/images/grpc.png)

## Websocket

![Websocket](assets/images/ws.png)

## GraphQL

Deed haven't supported instrospect yet.

![GraphQL](assets/images/graphql.png)

# Releases

The asset is `deed-<version>-macos-arm64.zip`.

The app is **ad-hoc signed** (not notarized with a Developer ID), so on first launch
macOS Gatekeeper blocks it. To open it:

* Right-click the app → **Open** → **Open**, or
* Remove the quarantine flag: `xattr -dr com.apple.quarantine /Applications/deed.app`
