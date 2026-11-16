# Android Custom Line Protocol Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add gRPC, Hysteria2, TUIC, and WireGuard custom lines to Android, with clear in-app and README support notes.

**Architecture:** Keep Xray for existing formats, gRPC, and WireGuard. Add a pinned sing-box runtime for Hysteria2 and TUIC. Both engines implement the existing `Line` lifecycle and dial only through their configured outbound; new line data remains backward-compatible in `lines.json`.

**Tech Stack:** Go helper, Xray-core, sing-box, Android Java / AndroidX, JSON line store.

**Spec:** `docs/superpowers/specs/2026-11-16-android-more-line-protocols-design.md`

## Global Constraints

- Keep VLESS, Trojan, VMess, and Shadowsocks links working.
- gRPC is a transport; Hysteria2, TUIC, and WireGuard are protocol profiles.
- Support Hysteria2 URI schemes `hysteria2://` and `hy2://`; TUIC URI links; WireGuard single-peer `.conf` / `wg-quick` files.
- Do not accept TLS certificate bypass options.
- Route WireGuard website DNS through its configured DNS server and WireGuard outbound; reject unsafe profiles.
- Do not request Android VPN permission or permit line failure to fall back to direct browsing.
- Preserve existing line IDs and records; store WireGuard as an internal base64url value in the existing `link` field, so no line JSON schema migration is needed.
- No tests will be added or run unless the user asks; perform an APK build for compilation verification.

## Review Focus

- Existing saved lines and subscription refresh keep selection and line IDs stable: validate via backward-compatible JSON fields in Task 3 and inspect generated migration behavior.
- User-supplied URIs with malformed credentials or unknown behavior-changing fields are rejected with actionable errors: enforce in Task 1 and Task 2 parsers.
- WireGuard website DNS cannot escape the tunnel: configure and verify the Xray DNS/routing graph in Task 1; reject profiles if that graph cannot be guaranteed.
- sing-box startup failure, helper shutdown, and auto-selection do not leak listeners/processes or route directly: implement lifecycle cleanup in Task 2 and inspect failure paths.
- File import of malformed, multi-peer, or oversized WireGuard configs does not crash or partially save: enforce size and structure checks in Task 3.

---

### Task 1: Xray gRPC and WireGuard profiles

**Files:**
- Modify: `warp/links.go`
- Modify: `warp/vless.go`
- Create: `warp/wireguard_profile.go`
- Modify: `warp/main.go`
- Modify: `warp/modes.go`

**Interfaces:**
- Preserve `parseLink(link string) (*core.Config, string, error)` for URI protocols.
- Add a WireGuard parser that converts a validated single-peer profile to an Xray `core.Config` and server endpoint; use `wireguard://<base64url wg-quick text>` only as the app-private link representation.
- Keep `Line.Dial`, `Line.Close/retire`, probing, and fail-closed behavior consistent for URI and WireGuard profiles.

- [x] **Step 1: Enable gRPC transport generation**

Import Xray's gRPC transport config, register its transport package, map URI `serviceName` and optional `authority`, and remove the explicit gRPC rejection. Leave TLS verification enabled and retain existing transport/security compatibility checks.

- [x] **Step 2: Add WireGuard config parsing and Xray outbound generation**

Parse one `[Interface]` and one `[Peer]` with `PrivateKey`, `Address`, `DNS`, `PublicKey`, `Endpoint`, and `AllowedIPs`; accept optional `PresharedKey`, `PersistentKeepalive`, and `MTU`. Reject repeated peers, duplicate required keys, missing DNS, invalid key encodings, and invalid endpoint/CIDR values. Build the Xray WireGuard outbound plus DNS app/routing so website DNS uses the configured resolver through that outbound. Endpoint host lookup may use the Android resolver before tunnel startup.

- [x] **Step 3: Add WireGuard start-time endpoint resolution**

Keep parse-only and probe on the existing single-line `ULB_PROBE` path using the canonical base64url internal URI. Before starting a live Xray profile, resolve a hostname Endpoint once through `net.DefaultResolver` (Android's device DNS); preserve the original profile record and route all website DNS through Xray's tagged DNS path over the WireGuard outbound.

- [x] **Step 4: Preserve close and failure behavior**

Ensure Xray instances created for WireGuard are closed on validation/probe failure and retired through the same lifecycle as existing lines. Confirm DNS setup only resolves the remote endpoint before tunnel startup and does not resolve website destinations directly.

**Verification:** Build the Go helper for Android ABIs in Task 5. Do not add or run tests unless the user asks.

### Task 2: Hysteria2 and TUIC sing-box runtime

**Files:**
- Modify: `warp/go.mod`
- Modify: `warp/go.sum`
- Create: `warp/singbox_line.go`
- Modify: `warp/links.go`
- Modify: `warp/vless.go`
- Modify: `warp/main.go`
- Modify: `warp/modes.go`

**Interfaces:**
- Add a typed parsed-profile representation for Hysteria2 and TUIC with scheme, name, server, port, credentials, TLS, and supported transport options.
- Add a line engine interface used by `Line` so Xray and sing-box profiles expose the same `Dial`, health check, and close behavior.
- Keep Xray as the default engine for all existing URI formats, gRPC, and WireGuard.

- [x] **Step 1: Pin sing-box and register only required outbound features**

Choose a stable sing-box version compatible with the repo Go toolchain; minimize imported packages and enabled build tags to Hysteria2, TUIC, TLS, QUIC, and SOCKS inbound support. Record the exact pin and third-party license location.

- [x] **Step 2: Parse and validate Hysteria2 URIs**

Accept `hysteria2://` and `hy2://`; parse password, host/port, SNI, and Salamander obfuscation. If SNI is absent, use the URI hostname. Reject insecure TLS flags and unsupported behavior-changing query fields. Produce one sing-box outbound config.

- [x] **Step 3: Parse and validate TUIC URIs**

Parse UUID/password, host/port, SNI, ALPN, congestion control, and UDP relay mode. Default absent SNI to the URI hostname and use secure TLS verification. Reject unknown behavior-changing query fields. Produce one sing-box outbound config.

- [x] **Step 4: Start and stop sing-box line sessions**

Create an in-process sing-box instance with the validated single outbound and no inbound. Call that configured outbound's `DialContext` from the common line engine. Close the sing-box instance on every startup failure, line switch, and helper shutdown path; this removes a secondary listener and leaves no direct outbound configured.

- [x] **Step 5: Integrate engine choice with auto-selection, probes, and subscriptions**

Dispatch Hysteria2/TUIC links to sing-box and existing formats to Xray during parse-only, probe, `StartLine`, `StartAuto`, and subscription refresh. Keep auto-selection comparisons and fallback behavior inside the browser line set; do not introduce direct-network fallback.

**Verification:** Build the Go helper in Task 5. Inspect Go module licenses and final linker/import set; do not add or run tests unless the user asks.

### Task 3: Android line storage and import UI

**Files:**
- Modify: `android/app/src/main/java/net/freedom8964/ulb/Lines.java`
- Modify: `android/app/src/main/java/net/freedom8964/ulb/LinesDialog.java`
- Modify: `android/app/src/main/java/net/freedom8964/ulb/MainActivity.java`

**Interfaces:**
- Keep `Lines.Line` JSON schema unchanged; old entries remain valid. Add `Lines.wireGuardLink(config)` to encode the app-private canonical URI.
- Pass the canonical URI through existing helper parse-only validation and line selection.
- Keep existing `-probe -parse-only` behavior for URL links.

- [x] **Step 1: Add canonical WireGuard profile record**

Encode UTF-8 config using URL-safe Base64 without padding and prefix it with `wireguard://`. Store it in the existing `link` field using the normal line add/save code; existing URI entries and subscription fields remain untouched.

- [x] **Step 2: Add WireGuard file import**

Add an Android document picker for `.conf` / text files. Enforce a 64 KiB maximum, read UTF-8, encode the internal URI, pass it to the existing helper parse-only mode, and save only after validation succeeds. On import reject incomplete, duplicate, malformed, multi-peer, or missing-DNS profiles with a clear message.

- [x] **Step 3: Update protocol labels and UI support note**

Show Hysteria2, TUIC, and WireGuard as protocol names; label gRPC only as a transport. Replace the existing support hint with the exact supported schemes, TCP/WS/HTTPUpgrade/XHTTP/gRPC transports, TLS/REALITY limits, WireGuard file requirements, and subscription limitation. Include a concise warning that WireGuard endpoint host resolution may use the Android network during startup while website DNS remains tunnel-routed.

- [x] **Step 4: Keep selection, rename, removal, and subscription paths stable**

Ensure WireGuard profiles can be selected, renamed, removed, and stored alongside URI lines using the existing `link` field. Update `protocolOf` / display helpers for `wireguard://`. Subscriptions remain public-link-only; explain that WireGuard configs are imported one at a time.

**Verification:** Review existing and new line JSON paths and build the Android APK in Task 5. Do not add or run tests unless the user asks.

### Task 4: Android README and dependency notices

**Files:**
- Modify: `android/README.md`
- Modify: `README.md`
- Modify: `FULL_VERSION_GUIDE.md` or a dedicated third-party notices file if required by the sing-box license

- [x] **Step 1: Document exact compatibility**

List link schemes, transport and security combinations, WireGuard file scope, unsupported options, DNS behavior, subscription limitations, and measured app/APK size change. Distinguish protocols from transports.

- [x] **Step 2: Add sing-box license attribution**

Follow the repository's existing third-party notice pattern and include all required notices for the pinned modules.

### Task 5: Build Android helper and APKs

**Files:**
- Modify only if required: `android/build-apk.sh`, Gradle packaging config, or Go build tags.

- [x] **Step 1: Build the Go helper for all Android ABIs**

Run `go build` with `GOOS=linux` for `arm64-v8a`, `armeabi-v7a`, and `x86_64`, using the exact toolchain in `warp/go.mod`. Ensure each output remains executable as `libulbwarp.so`.

- [x] **Step 2: Assemble debug APKs**

Run `GRADLE=<gradle-9-path> android/build-apk.sh debug`. Confirm ABI APKs and universal APK are produced, and report sizes compared with the prior build if available.

- [x] **Step 3: Review final diff and network failure paths**

Run `git diff --check`; inspect the final protocol support copy and verify no code path silently switches a failed proxy connection to direct browsing.
