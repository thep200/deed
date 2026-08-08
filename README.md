![Deed main](assets/images/main.png)

<h1 align="center">Deed ✨</h1>
<p align="center"> A lightweight, native macOS API client in retro Mac OS 9 style that speaks REST/HTTP, SSE, gRPC, WebSocket, GraphQL, SOAP, Kafka, and LDAP from a single <=50 MB binary. </p>

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
  - [Secrets at rest](#secrets-at-rest)
- [Collections](#collections)
  - [Ordering and drag-and-drop](#ordering-and-drag-and-drop)
- [Request type](#request-type)
  - [How a send works](#how-a-send-works)
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
  - [SOAP](#soap)
  - [Kafka](#kafka)
    - [Producer](#producer)
    - [Consumer](#consumer)
    - [Avro (Schema Registry)](#avro-schema-registry)
  - [LDAP](#ldap)
- [Releases](#releases)

# Overview

Supports:

- RESTFUL / HTTP
- SSE
- gRPC (unary, server-streaming, client-streaming, bidi)
- WebSocket
- GraphQL (query, mutation, subscription, schema introspection)
- SOAP (1.1 & 1.2)
- Kafka (producer & consumer, Avro via Schema Registry)
- LDAP (search, group check, credential check; StartTLS & paged results)

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
- GraphQL (incl. schema introspection — Schema tab)
- SOAP 1.1 / 1.2 (XML pretty-print, fault-aware)
- Kafka (incl. Avro values via Confluent Schema Registry)
- LDAP / LDAPS (StartTLS, paged results, group + credential checks)
- Auth: Basic, Bearer, OAuth2 (client credentials / password, token fetched & cached automatically)
- Lazy load collection tree, drag-and-drop ordering
- Request editor (JSON / XML syntax highlight)
- Response viewer (JSON / XML syntax highlight + pretty-print)
- Environment & Alias, with a shared `Global` env and AES-256-GCM encryption for the values you mark
- Import curl, grpcurl, GraphQL, ldapsearch

# Techstack

- C++17 (cross-platform)
- Objective-C++ / AppKit
- Scintilla
- libcurl/cpr
- gRPC C++ + Protobuf
- librdkafka
- Apache Avro (avro-cpp)
- OpenLDAP (libldap)
- OpenSSL
- nlohmann/json
- CMake + Ninja

# Configuration

## Global config

The gear button opens **Settings**, a plain JSON editor:

```json
{
  "font_name": "Monaco 9",
  "font_size": 21,
  "ram_cache_size": 32,
  "disk_cache_size": 256,
  "encryption_key": ""
}
```

Deed caches responses in two tiers, RAM and disk:

* The RAM cache can be set up to **128 MB**.
* The disk cache can be set up to **512 MB**.

`ram_cache_size` and `disk_cache_size` are the levels you choose; anything higher than the limits above is capped automatically. `encryption_key` is covered in [Secrets at rest](#secrets-at-rest).

## Environments

Environments live in `environments/` inside the collection, one JSON file per environment. **Manage env** (next to Back in Settings) opens the grid where you add aliases and switch the active environment.

`Global.json` is a reserved base environment: its keys are always in scope, whichever environment is active, and the active one wins on a key they both define. It never shows up in the environment selector — edit it in the grid like any other. When you import a cURL, grpcurl, GraphQL, or ldapsearch command, Deed automatically creates the matching aliases for you.

Use `{{key}}` anywhere a value is accepted (URL, headers, query, auth, gRPC metadata, LDAP params, …) to insert an environment value. If a key doesn't exist it's left as-is (`{{key}}`) and flagged; if it exists but is empty it becomes an empty string.

An environment file is just a list of keys:

```json
{
  "schemaVersion": 1,
  "keys": [
    {
      "key": "baseUrl",
      "value": "https://api.example.com",
      "enabled": 1,
      "secret": 0
    },
    {
      "key": "token",
      "value": "enc:v1:9Xk2…",
      "enabled": 1,
      "secret": 1
    }
  ]
}
```

## Secrets at rest

Set `encryption_key` in Settings to any passphrase, then flip the **Enc** toggle on the aliases you want protected. Those values are written to disk as `enc:v1:<base64>` — AES-256-GCM, key derived with SHA-256 — so you can commit the whole collection without leaking tokens. Everything else stays plain text.

- Encryption is **per alias**: nothing happens to the rest of the file, and the toggle is the only thing that triggers it. Changing `encryption_key` later re-encrypts nothing — re-toggle **Enc** on the aliases you want moved to the new key.
- Values whose plaintext hasn't changed keep their stored ciphertext byte for byte, so saving an environment doesn't churn the file in git.
- Open the collection with the wrong (or no) passphrase and the value stays visible as `enc:v1:…` instead of being lost or blanked. Sending while a `{{var}}` is still unreadable shows a warning toast — the request goes out with the ciphertext in place, so fix the passphrase before trusting the result.

> The passphrase itself lives in your local app config, never in the collection. Request files (OAuth2 client secrets, Kafka registry credentials, LDAP bind passwords, …) are still stored as plain text — put those in an environment alias and reference `{{var}}`.

# Collections

A collection is just a folder. Every request is one small JSON file, every folder is a real directory, so the whole thing lives in git and diffs cleanly.

Deed reads the tree from filenames alone — it never opens a request file to draw the tree — so a collection with thousands of requests still expands instantly. Filenames follow one grammar:

```
a3+k3n8qv2rt5wd_http_get_list-users.json     <order>+<id>_<type>[_<method>]_<slug>.json
a4+auth                                       <order>+<slug>          (folders)
```

The `<id>` is stable: renaming, moving, or reordering a request never changes it, so the app can always find the request you had open. `.session/` and `.secrets/` are added to the collection's `.gitignore` automatically.

## Ordering and drag-and-drop

Folders and requests share one order sequence per level, held in the `<order>+` prefix — a fractional index, so **moving one entry renames one file** and leaves its siblings untouched.

- Drag a row and drop it between two rows to place it exactly there. A request row splits at its midline — upper half inserts above it, lower half below. A folder row keeps its middle for "move inside this folder"; its top edge inserts above it, and its bottom edge inserts below it while the folder is collapsed (an open folder keeps its whole body as "move inside", since the slot after it sits under everything it contains). Dropping in the empty area below the last row appends at the top level.
- New requests, new folders, and imports land at the **bottom** of their level. A duplicate lands directly under its original.
- Renaming or saving a request keeps its slot.
- A collection created before ordering existed has no keys at all. The first drop into such a folder assigns keys to every entry in it, in the order already on screen, so nothing visibly jumps — after that it's one rename per move like everywhere else.

# Request type

Give the app 30 minutes hands-on and this will all click — but here's the reference.

Each request has editing tabs on the **left** and a read-only response on the **right**. Whatever you fill into the tabs is saved to one small JSON file per request, so you can keep an entire collection in a folder and sync it with git.

> Files don't support comments. To turn a single line in **Headers**, **Query**, or **Metadata** on or off, set its `enabled` to `0` instead of deleting it.

## How a send works

Every request type follows the same path when you press **Send**:

1. Deed reads the URL field and the left-hand tabs (bad JSON in a tab shows a toast and jumps you to the offending tab — nothing is sent).
2. Every `{{key}}` is replaced with its value from the active environment.
3. Auth is applied and disabled lines are dropped (an `oauth2` auth fetches its token here — cached, so only the first send of a session pays the extra round-trip).
4. The request goes out on a background worker, so the UI never freezes and you can keep browsing the collection.

What comes back depends on the request's shape:

- **One-shot requests** (HTTP, SOAP, GraphQL query/mutation, gRPC unary & client-streaming, Kafka producer, LDAP) show a single response on the right, plus status, elapsed time, and size in the status bar.
- **Streaming requests** (SSE, gRPC server-streaming/bidi, WebSocket, GraphQL subscription, Kafka consumer) open a live session: each incoming message is appended to the right pane as it arrives, with running counters for elapsed / events / size. The session stays open until the server ends it or you press **Cancel** (for WebSocket that's a polite disconnect).

Responses are also cached (RAM + disk, see [Global config](#global-config)), so reopening a request shows its last response instantly — even after an app restart.

## Config

The **Config** tab (the last left-hand tab, on every request type) holds two settings shared by all types:

```json
{
  "timeout_ms": 1800000,
  "tls": true
}
```

- `timeout_ms` — how long to wait before giving up. For HTTP/GraphQL it's the request timeout, for gRPC the deadline, for WebSocket the idle timeout. For a Kafka producer the effective delivery timeout is the **smaller** of this and the Kafka tab's `messageTimeoutMs`.
- `tls` — verify the server's TLS certificate (for gRPC, connect over a secure channel; for LDAP it also covers the StartTLS upgrade). The Kafka Config tab only has `timeout_ms` (no TLS toggle yet).

New requests start with the defaults from `.env` (`DEFAULT_TIMEOUT_MS`, `VERIFY_TLS`) — 30 minutes and TLS verification on, out of the box.

> The **Pretty** button has four modes — **Pretty / Raw / Encode / Decode**. Click into the pane you want it to act on first, then press it.

## RESTFUL HTTP

Paste a `curl` command straight into the URL field and Deed turns it into a new request automatically.

### Request Body

The **Body** tab button is a dropdown — click it to pick one of five modes: **JSON**, **Text**, **XML**, **File**, or **Form**. One mode is active at a time, and switching modes keeps what you typed in each as a draft, so you can flip back and forth without losing anything.

JSON (default; `{}` = no body):

```json
{
  "name": "deed"
}
```

Text / XML — sent verbatim, exactly as typed.

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

The **Auth** tab is one flat JSON object with a single `type` key: **none**, **basic**, **bearer**, or **oauth2**. All fields sit next to `type` — no nesting.

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
  "username": "admin",
  "password": "s3cret"
}
```

Bearer — a token:

```json
{
  "type": "bearer",
  "token": "{{token}}"
}
```

OAuth2 — Deed fetches the token itself, caches it in RAM for the session, and refreshes it near expiry. Works for HTTP, GraphQL (both transports), and WebSocket; the request goes out as a plain `Authorization: Bearer …`:

```json
{
  "type": "oauth2",
  "grant": "client_credentials",
  "tokenUrl": "https://idp.example.com/connect/token",
  "clientId": "{{clientId}}",
  "clientSecret": "{{clientSecret}}",
  "scope": "api",
  "clientAuth": "header"
}
```

- `grant` — `"client_credentials"` (default) or `"password"` (then add `"username"` / `"password"` fields). Authorization-code/PKCE is not supported yet.
- `clientAuth` — how the client authenticates at the token endpoint: `"header"` (HTTP Basic, the RFC default) or `"body"` (`client_id`/`client_secret` as form fields — some IdPs insist).
- `scope` is optional; `{{var}}` works in every field, including the secret.
- A token-fetch failure fails the send with an `oauth2 token: …` message. WebSocket fetches the token once at the handshake — a token expiring mid-session does not re-handshake.

Need an API key? That's just a header (or query param) — put it in the **Headers** or **Query** tab directly, e.g. header `X-API-Key: {{apiKey}}`. The former `apikey` auth type was removed for exactly this reason.

> Older saved requests still load: the previous nested shape (`{"type":"basic","basic":{…}}`) is read transparently and rewritten flat on the next save; a saved `"type": "apikey"` falls back to `none` (move the key/value into **Headers**/**Query**).

### Response Body

The **Response** tab (read-only) shows the response body, pretty-printed when it's JSON.

### Response Headers

The right-hand **Headers** tab (read-only) lists the response headers exactly as the server returned them. Any `Set-Cookie` values also appear in the **Cookie** tab.

### Response Request

The right-hand **Request** tab (read-only) shows the exact request that was sent — after variables are substituted, auth is applied, and disabled lines are removed. Handy for checking what the server actually received.

## SSE

Server-Sent Events is simply an HTTP request that streams. Add an `Accept: text/event-stream` header (enabled) to a plain HTTP request and Deed does the rest: instead of waiting for one response, the right pane turns into a live event log and each `data:` payload is appended as it arrives. Press **Cancel** to stop listening.

![SSE](assets/images/sse.png)

## gRPC

**URL field** — the target as `host:port` (no scheme), e.g. `localhost:8765`. Whether the channel is plaintext or TLS follows the `tls` flag in the **Config** tab.

**Picking the RPC** — Deed needs the schema to encode your message. Two sources, chosen with the proto dropdown next to the URL:

- **Reflection** (default) — the server must have gRPC reflection enabled. Click the method dropdown and Deed queries the server and lists every `service/method` it exposes; pick one.
- **.proto** — no reflection? Choose `.proto` and a file picker opens; point it at your local `.proto` file and Deed parses the services out of it instead.

**Tabs:**

- **Message** — the request message as JSON (field names as in the `.proto`). For **client-streaming / bidi** methods, put a JSON **array** here — each element is sent as one message, e.g. `[{"n": 1}, {"n": 2}, {"n": 3}]`.
- **Metadata** — key/value lines sent as gRPC metadata, same format as HTTP headers (`enabled: 0` to skip a line). Keys must be lowercase (`a-z 0-9 - _ .`). There is no Auth tab for gRPC: call-level auth **is** metadata — new requests come seeded with disabled example lines (`authorization: Bearer <token>`, `x-api-key`, `x-request-id`); flip `enabled` to `1` and fill in the value to send one. Channel security (TLS) is the `tls` flag in **Config**.
- **Config** — `timeout_ms` is the gRPC deadline; `tls` picks secure vs plaintext channel.

**Responses** — unary and client-streaming show one JSON response. Server-streaming and bidi stream into the right pane message by message; press **Cancel** to hang up. Very long streams are truncated for safety after 100k events or 64 MB (tunable in `.env`: `STREAM_MAX_EVENTS`, `STREAM_MAX_BYTES_MB`).

You can also paste a `grpcurl` command into the URL field and Deed imports it as a new request.

![gRPC](assets/images/grpc.png)

## Websocket

**URL field** — a `ws://` or `wss://` endpoint.

**Tabs:**

- **Message** — the frame to send. If it's non-empty it is sent automatically right after connecting; after that, edit it and press **Send** again to push the current text as a new frame through the open session — as many times as you like.
- **Headers** — extra handshake headers (the HTTP upgrade request).
- **Auth** — applied to the handshake, same three types as HTTP.

**Session** — pressing **Send** connects. The right pane logs every frame in and out, live. **Cancel** performs a graceful disconnect (close code 1000). Deed keeps the connection healthy with automatic keepalive pings and closes it if the server goes silent too long (tunable in `.env`: `WS_PING_INTERVAL_MS`, `WS_IDLE_TIMEOUT_MS`, …).

![Websocket](assets/images/ws.png)

## GraphQL

**URL field** — the HTTP endpoint (e.g. `https://api.example.com/graphql`).

**Tabs:**

- **Query** — the GraphQL document (`query { … }`, `mutation { … }`, or `subscription { … }`).
- **Variables** — the variables object as JSON, e.g. `{"id": 42}`.
- **Headers / Auth** — same as HTTP.

Queries and mutations are sent over HTTP and show one response. A **subscription** runs over WebSocket using the `graphql-transport-ws` protocol (the legacy `subscriptions-transport-ws` is also supported) and streams events into the right pane until you press **Cancel** — set `"subTransport": "ws"` in the saved request file to switch the transport over.

Paste a bare GraphQL document (`query { … }` / `mutation { … }` / `subscription { … }`) into the URL field and Deed imports it as a new GraphQL request — fill in the endpoint afterwards.

**Schema (introspection)** — the right pane has a **Schema** tab. First click POSTs the standard introspection query to the endpoint (with the request's headers/auth) and renders the server schema as SDL; the **Pretty/Raw** button toggles between SDL and the raw introspection JSON. The result is cached per request and refetched after you edit the URL, switch requests, or a send fails — just click the tab again. Servers with introspection disabled show the error as a toast.

![GraphQL](assets/images/graphql.png)

## SOAP

**URL field** — the service endpoint (e.g. `http://www.dneonline.com/calculator.asmx`).

**Tabs:**

- **Envelope** — the full SOAP envelope as raw XML (nothing is wrapped or generated for you — what you type is what's POSTed).
- **Headers / Auth** — same as HTTP (OAuth2 works too).
- **Soap** — the protocol knobs:

  ```json
  {
    "action": "http://tempuri.org/Add",
    "version": "1.1"
  }
  ```

  | version | Content-Type | action |
  |---|---|---|
  | `"1.1"` | `text/xml; charset=utf-8` | `SOAPAction: "<action>"` header (always sent, even empty) |
  | `"1.2"` | `application/soap+xml; charset=utf-8` | `action="<action>"` inside the Content-Type |

  Setting your own `Content-Type` or `SOAPAction` in the **Headers** tab overrides Deed's.

**Send** POSTs the envelope and shows the response. The Envelope tab and any XML response are syntax-highlighted (tags/attributes/values); press **Pretty** to indent the XML. A SOAP Fault usually arrives as HTTP 500 with a `<soap:Fault>` body; the response is shown as-is. WSDL browsing isn't supported yet — grab the operation's action/namespace from your service's WSDL by hand.

## Kafka

**URL field** — the bootstrap servers, comma-separated `host:port` pairs, e.g. `localhost:9092` or `broker1:9092,broker2:9092`.

A **Prod / Cons** toggle next to the URL switches the request between producer and consumer — each side keeps its own tabs.

### Producer

- **Message** tab:

  ```json
  {
    "key": "user-42",
    "value": { "name": "deed" },
    "headers": []
  }
  ```

  - `key` — optional message key (empty = no key; Kafka then picks the partition round-robin).
  - `value` — the payload, written as real JSON right in the editor.
  - `headers` — key/value lines, same format as HTTP headers.

- **Kafka** tab — the producer settings:

  ```json
  {
    "topic": "events",
    "partition": -1,
    "acks": "all",
    "compression": "none",
    "messageTimeoutMs": 30000,
    "lingerMs": 0,
    "retries": 3,
    "idempotence": false,
    "clientId": "deed",
    "extra": []
  }
  ```

- `partition: -1` lets Kafka choose.
- `acks` is `"0"` / `"1"` / `"all"`. 
- `compression` is `none/gzip/snappy/lz4/zstd`.
- `extra` passes any raw librdkafka property as key/value lines.

**Send** produces one message and shows a single delivery report (topic / partition / offset / latency). **Cancel** aborts a delivery that's stuck on an unreachable broker.

### Consumer

One **Kafka** tab:

```json
{
  "topics": ["events"],
  "group": "",
  "offsetReset": "latest",
  "partition": -1,
  "autoCommit": true,
  "maxMessages": null,
  "pollTimeoutMs": 500,
  "clientId": "deed",
  "extra": []
}
```

- `group` — consumer group id. **Leave it empty** and Deed generates a fresh `deed-tail-…` group per run, so `offsetReset` always applies.
- `offsetReset` — `"latest"` (default) tails only new messages; `"earliest"` reads the topic **from the beginning**. Note Kafka semantics: this only applies when the group has no committed offsets — another reason to leave `group` empty.
- `partition` — `-1` subscribes to all partitions; a number ≥ 0 pins to that one partition.
- `maxMessages` — `null` tails forever; a number stops after N records.

**Send** starts tailing: records stream into the right pane live (topic, partition, offset, key, value, headers, timestamp). Press **Cancel** to stop.

### Avro (Schema Registry)

Deed speaks the Confluent wire format (magic byte + schema id + Avro binary) for message **values** (keys stay plain strings).

**Producer** — you still type plain JSON in the Message tab; set two keys in the **Kafka** tab and Deed serializes it to Avro on send:

```json
{
  "topic": "events",
  "valueFormat": "avro",
  "schemaRegistry": { "url": "http://localhost:8081", "username": "", "password": "" }
}
```

Deed fetches the **latest** schema registered for the subject `<topic>-value` (TopicNameStrategy) and encodes your JSON against it. There's no schema registration in Deed — register the schema first (CI, `curl`, or the console tools). Avro **union** fields use the Avro-JSON encoding — `{"string": "hello"}` or `null`, the same convention `kafka-avro-console-producer` expects.

**Consumer** — add the same `schemaRegistry` block to the consumer's Kafka tab and decoding is automatic: any record whose value carries the Confluent framing is decoded and shown as JSON, annotated with `"valueEncoding": "avro (id N)"`. Records that aren't Avro-framed display verbatim as before. If the registry is unreachable or the schema can't decode the bytes, the record still streams — verbatim, marked `undecoded` — and Deed retries the registry at most once per schema id every 30 seconds.

> Registry credentials are stored in the request file as plain text — prefer `{{vars}}` from an environment, and mark them **Enc** (see [Secrets at rest](#secrets-at-rest)).

## LDAP

**URL field** — `ldap://host:389` or `ldaps://host:636`. No method dropdown; there are no headers and no Auth tab, because LDAP credentials are the bind DN and password, which live in **Params**.

**Tabs** — just **Params** and **Config**. Everything the request does is one JSON object:

```json
{
  "startTls": false,
  "bindDn": "cn=admin,dc=example,dc=com",
  "bindPassword": "{{ldapPassword}}",
  "baseDn": "dc=example,dc=com",
  "scope": "sub",
  "filter": "(uid=bob)",
  "attributes": [],
  "group": "",
  "testPassword": "",
  "sizeLimit": 100,
  "timeLimit": 10,
  "pageSize": 500
}
```

- `bindDn` empty = anonymous bind. `attributes` empty = ask for everything.
- `scope` — `base`, `one`, or `sub`. `filter` accepts a bare `uid=bob`; Deed wraps it in parentheses. Empty = `(objectClass=*)`.
- `startTls` upgrades a plain `ldap://` connection to TLS (ignored for `ldaps://`, which is already encrypted). Certificate verification follows the `tls` flag in **Config**.
- `sizeLimit` / `timeLimit` — server-side caps, `0` = no client limit. `pageSize` sends an RFC 2696 paged-results control, marked non-critical so servers that don't page still answer in one go; `0` turns it off.

Two optional helpers turn a search into a check:

- `group` — a group DN. It's ANDed into the filter as `memberOf`, and if nothing matches Deed re-runs the plain filter to tell "no such user" apart from "user exists, not in the group".
- `testPassword` — after the search finds **exactly one** entry, Deed binds again as that entry's DN with this password on a fresh connection. That's the search-then-bind an app does when a user logs in.

**Response** — one JSON verdict:

```json
{
  "verdict": "MATCH",
  "matched": 1,
  "resultCode": 0,
  "diagnostic": "",
  "pages": 1,
  "entries": [
    { "dn": "uid=bob,dc=example,dc=com",
      "attributes": { "cn": ["Bob"], "mail": ["bob@example.com"] } }
  ]
}
```

`verdict` is one of `MATCH`, `NO_MATCH`, `NOT_IN_GROUP`, `CREDENTIALS_OK`, `INVALID_CREDENTIALS`, or `AMBIGUOUS` (more than one entry matched, so Deed refuses to guess whose password to test). Binary attributes such as `jpegPhoto` come back base64 with a `;base64` suffix on the key. The status bar shows the LDAP result code, not an HTTP one — `rc=0 · MATCH (1)` — and only `MATCH` / `CREDENTIALS_OK` count as green, since `rc=49` (invalid credentials) is a perfectly valid answer that still means "no".

Paste an `ldapsearch` command or an RFC 4516 URL (`ldap://host/base?attrs?scope?filter`) into the URL field and Deed imports it as a new request; `-Z`/`-ZZ` map to `startTls` and `-E pr=<n>` to `pageSize`. Flags Deed doesn't know are reported as skipped instead of failing the import.

> The group check relies on the `memberOf` attribute. Active Directory has it out of the box; OpenLDAP needs the `memberof` overlay enabled, and without it a real member is reported as `NOT_IN_GROUP`.

# Releases

The asset is `deed-<version>-macos-arm64.zip`.

The app is **ad-hoc signed** (not notarized with a Developer ID), so on first launch
macOS Gatekeeper blocks it. To open it:

* Right-click the app → **Open** → **Open**, or
* Remove the quarantine flag: `xattr -dr com.apple.quarantine /Applications/deed.app`
