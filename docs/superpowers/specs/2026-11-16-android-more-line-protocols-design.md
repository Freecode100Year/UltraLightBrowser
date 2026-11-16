# Android custom line protocol expansion

## Intent

Expand Android's custom network lines so users can import more common proxy protocols without silently changing the browser's network routing. Make supported formats and important compatibility limits visible in the line manager and Android README.

## Goals

- Keep existing VLESS, Trojan, VMess, and Shadowsocks links working.
- Enable gRPC as a transport for existing Xray protocols.
- Add Hysteria2 (`hysteria2://` and `hy2://`) and TUIC share links.
- Add WireGuard by importing a standard single-peer `wg-quick` configuration file; WireGuard has no single interoperable share-link URI.
- Keep WebView on the existing helper SOCKS5 listener so proxying, line selection, subscriptions, and probe behavior continue to work. Protocol engines dial destinations through only their configured outbound.
- Show protocols, accepted transports, import format, and unsupported options in the Android UI and `android/README.md`.
- Preserve fail-closed behavior: a failed/malformed line never falls back to direct browsing.

## Non-goals and constraints

- Do not add Chrome/Edge extension support or change Android WebView.
- Do not replace the WARP implementation or alter Windows line support.
- Do not import untrusted remote configuration code. Link parsing and config-file parsing remain local.
- Do not accept insecure TLS certificate bypass settings.
- Do not claim full compatibility with every client-specific URI or subscription format.
- The Android line store stays private to the application. Subscription support remains text share-link lists / Base64 lists; arbitrary YAML is out of scope.

## Current behavior

Android uses the repository's Go helper as a local SOCKS5 proxy. `warp/links.go` parses VLESS, Trojan, VMess, and Shadowsocks, with TCP, WebSocket, HTTPUpgrade, and XHTTP transports. gRPC is explicitly rejected. `LinesDialog` accepts text links and validates them by invoking the helper's parse-only mode. The Go module is based on Xray-core; Android builds the same helper for each ABI.

## Approaches considered

1. **Add sing-box as a second engine for Hysteria2 and TUIC while keeping Xray for existing links, gRPC, and WireGuard (selected):** limits changes to established Xray parsing and preserves current behavior. It duplicates protocol runtimes and may materially increase APK size and maintenance.
2. **Replace Xray line execution with sing-box:** offers a unified protocol catalog, but requires rewriting link parsing, config generation, probing, and protocol parity checks; the risk to existing VLESS/REALITY/XHTTP and subscriptions is higher.
3. **Implement new protocols separately:** could avoid a second general-purpose core, but would require maintaining protocol-specific implementations and TLS/QUIC behavior in this project; less maintainable and higher correctness risk.

## Proposed design

- Keep Xray as the engine for existing formats, gRPC transport, and WireGuard. Add gRPC transport configuration to supported link parsing and pass the service name and authority fields using Xray's documented settings. Do not enable insecure certificates as a side effect.
- Integrate a pinned sing-box core only for Hysteria2 and TUIC line sessions. Generate an in-memory config with one outbound and no inbound; dial through that outbound's `DialContext` from the existing line engine interface. Keep the Android WebView on the helper's existing SOCKS listener. This avoids a second listener and leaves no direct outbound configured.
- Parse standard Hysteria2 URI variants (`hysteria2://`, `hy2://`) and TUIC URIs into typed profiles. Support credentials, host/port, `sni`, Hysteria2 Salamander obfuscation, and common TUIC congestion/UDP relay/ALPN options. Default missing TLS SNI to the URI hostname. Reject certificate bypass, malformed credentials, and unknown options that affect connection behavior with an actionable reason; do not silently weaken TLS.
- Parse WireGuard `.conf`/`wg-quick` files as a separate import path. Initial scope is one `[Interface]` and one `[Peer]`, required private/public keys, `Endpoint`, `AllowedIPs`, and `DNS`, plus optional `PresharedKey`, `PersistentKeepalive`, and MTU. Generate an Xray WireGuard outbound and route DNS queries through the configured resolver over the WireGuard outbound. The endpoint hostname may be resolved using the Android network before the tunnel is up; website DNS must not use that path. If the core cannot guarantee tunneled website DNS, reject the profile rather than silently leaking lookups. Route browser requests through the existing local SOCKS proxy without requesting Android VPN permission.
- Store WireGuard files as a canonical app-private `wireguard://<base64url(UTF-8 wg-quick text)>` value in the existing `link` field. This keeps old line records and line-selection paths unchanged. This scheme is internal to the app and is not presented as a public WireGuard share-link standard.
- Add an import affordance for WireGuard files alongside link paste. After local validation, convert the file into the canonical internal URI and pass it through the existing parse-only/probe path. Subscription import accepts the new URI schemes for Hysteria2/TUIC; WireGuard files are imported individually and are not subscription entries.
- Update the line manager's explanatory note and README with supported schemes, transport/security coverage, WireGuard file scope, unsupported cases, and any measured binary-size impact after implementation.

## Success criteria

- Existing custom line links remain loadable and can still be probed.
- gRPC links parse and form a connection using the service name and TLS server name provided by the link.
- Hysteria2 and TUIC profiles import, validate, probe, start, and stop through the same line lifecycle and helper SOCKS proxy path.
- A valid single-peer WireGuard file imports and can carry WebView TCP and DNS traffic through the helper; malformed, multi-peer, or DNS-unsafe files receive actionable errors. UI text explains that the server endpoint hostname may use the device resolver during tunnel setup.
- Switching/removing/updating subscriptions does not leave proxy processes or corrupt selected-line state.
- UI and README clearly distinguish share-link protocols from transports, list limits, and do not imply every upstream option is supported.
- If the additional core causes a significant APK-size increase, report per-ABI and universal APK sizes in the implementation summary.

## Open implementation decisions

- Exact sing-box version and the smallest stable package/API surface must be selected during the implementation plan. The dependency must be pinned and its licenses included in existing third-party notices.
- If the maintained sing-box APIs cannot dial through a configured outbound without adding an unsafe fallback, reject the profile rather than weakening the network boundary.
