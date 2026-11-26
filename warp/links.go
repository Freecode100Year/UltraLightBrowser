package main

// Share links for a line ("我的线路"): vless://, trojan://, vmess:// and ss://,
// over TCP, WebSocket, HTTPUpgrade or XHTTP, with TLS, REALITY or no
// security. Each link becomes one Xray outbound; nothing else (no inbounds, no
// routing) is configured, so every connection goes through that server.

import (
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"

	"github.com/xtls/xray-core/app/dispatcher"
	applog "github.com/xtls/xray-core/app/log"
	"github.com/xtls/xray-core/app/proxyman"
	_ "github.com/xtls/xray-core/app/proxyman/outbound"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/common/protocol"
	"github.com/xtls/xray-core/common/serial"
	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/shadowsocks"
	ss2022 "github.com/xtls/xray-core/proxy/shadowsocks_2022"
	"github.com/xtls/xray-core/proxy/trojan"
	"github.com/xtls/xray-core/proxy/vless"
	vlessout "github.com/xtls/xray-core/proxy/vless/outbound"
	"github.com/xtls/xray-core/proxy/vmess"
	vmessout "github.com/xtls/xray-core/proxy/vmess/outbound"
	"github.com/xtls/xray-core/transport/internet"
	grpcTransport "github.com/xtls/xray-core/transport/internet/grpc"
	"github.com/xtls/xray-core/transport/internet/httpupgrade"
	"github.com/xtls/xray-core/transport/internet/reality"
	"github.com/xtls/xray-core/transport/internet/splithttp"
	_ "github.com/xtls/xray-core/transport/internet/tcp"
	"github.com/xtls/xray-core/transport/internet/tls"
	"github.com/xtls/xray-core/transport/internet/websocket"
)

// params is the query of a share link (or the fields of a vmess:// JSON).
type params map[string]string

func (p params) get(k string) string { return strings.TrimSpace(p[k]) }

func queryParams(raw string) (params, error) {
	q, err := url.ParseQuery(raw)
	if err != nil {
		return nil, fmt.Errorf("线路参数格式不正确")
	}
	p := params{}
	for k, v := range q {
		if len(v) != 1 {
			return nil, fmt.Errorf("线路参数 %s 重复填写", k)
		}
		p[k] = v[0]
	}
	return p, nil
}

// parseLink turns a share link into an Xray config with a single outbound, and
// returns the server as host:port.
func parseLink(link string) (*core.Config, string, error) {
	scheme, _, hasScheme := strings.Cut(strings.ToLower(strings.TrimSpace(link)), "://")
	if hasScheme && (scheme == "hysteria2" || scheme == "hy2" || scheme == "tuic") {
		p, err := parseQUICLine(link)
		if err != nil {
			return nil, "", err
		}
		cfg, server := hysteriaConfig(p)
		return cfg, server, nil
	}
	if strings.EqualFold(strings.SplitN(link, "://", 2)[0], "wireguard") {
		return parseWireGuardLink(strings.TrimSpace(link))
	}
	proxy, stream, host, port, err := parseShare(strings.TrimSpace(link))
	if err != nil {
		return nil, "", err
	}
	cfg := &core.Config{
		App: []*serial.TypedMessage{
			// No logs: they would name every site visited.
			serial.ToTypedMessage(&applog.Config{ErrorLogType: applog.LogType_None, AccessLogType: applog.LogType_None}),
			serial.ToTypedMessage(&dispatcher.Config{}),
			serial.ToTypedMessage(&proxyman.OutboundConfig{}),
		},
		Outbound: []*core.OutboundHandlerConfig{{
			SenderSettings: serial.ToTypedMessage(&proxyman.SenderConfig{StreamSettings: stream}),
			ProxySettings:  proxy,
		}},
	}
	return cfg, net.JoinHostPort(host, strconv.Itoa(port)), nil
}

func parseShare(link string) (proxy *serial.TypedMessage, stream *internet.StreamConfig, host string, port int, err error) {
	scheme, _, ok := strings.Cut(link, "://")
	if !ok {
		return nil, nil, "", 0, fmt.Errorf("不是分享链接")
	}
	switch strings.ToLower(scheme) {
	case "vless":
		return parseVless(link)
	case "trojan":
		return parseTrojan(link)
	case "vmess":
		return parseVmess(link)
	case "ss":
		return parseSS(link)
	case "wireguard":
		return nil, nil, "", 0, fmt.Errorf("WireGuard 配置应通过文件导入")
	}
	return nil, nil, "", 0, fmt.Errorf("不支持 %s:// 链接", scheme)
}

func endpoint(host string, port int, account *serial.TypedMessage) *protocol.ServerEndpoint {
	return &protocol.ServerEndpoint{
		Address: xnet.NewIPOrDomain(xnet.ParseAddress(host)),
		Port:    uint32(port),
		User:    &protocol.User{Account: account},
	}
}

// hostPort reads the server of a URL-style link.
func hostPort(u *url.URL) (string, int, error) {
	host := u.Hostname()
	port, err := strconv.Atoi(u.Port())
	if host == "" || err != nil || port <= 0 || port > 65535 {
		return "", 0, fmt.Errorf("服务器地址或端口不正确")
	}
	return host, port, nil
}

func parseVless(link string) (*serial.TypedMessage, *internet.StreamConfig, string, int, error) {
	u, err := url.Parse(link)
	if err != nil || u.User == nil || u.User.Username() == "" {
		return nil, nil, "", 0, fmt.Errorf("vless:// 链接格式不正确")
	}
	host, port, err := hostPort(u)
	if err != nil {
		return nil, nil, "", 0, err
	}
	p, err := queryParams(u.RawQuery)
	if err != nil {
		return nil, nil, "", 0, err
	}
	stream, err := streamConfig(p, host, "none")
	if err != nil {
		return nil, nil, "", 0, err
	}
	flow := p.get("flow")
	if flow != "" && flow != vless.XRV {
		return nil, nil, "", 0, fmt.Errorf("不支持流控 %q", flow)
	}
	if flow != "" && (stream.ProtocolName != "tcp" || stream.SecurityType == "") {
		return nil, nil, "", 0, fmt.Errorf("流控 %s 只能用于 TCP + TLS / REALITY", flow)
	}
	enc := p.get("encryption")
	if enc == "" {
		enc = "none"
	}
	account := &vless.Account{Id: u.User.Username(), Flow: flow, Encryption: enc}
	out := &vlessout.Config{Vnext: endpoint(host, port, serial.ToTypedMessage(account))}
	return serial.ToTypedMessage(out), stream, host, port, nil
}

func parseTrojan(link string) (*serial.TypedMessage, *internet.StreamConfig, string, int, error) {
	u, err := url.Parse(link)
	if err != nil || u.User == nil || u.User.Username() == "" {
		return nil, nil, "", 0, fmt.Errorf("trojan:// 链接格式不正确")
	}
	host, port, err := hostPort(u)
	if err != nil {
		return nil, nil, "", 0, err
	}
	p, err := queryParams(u.RawQuery)
	if err != nil {
		return nil, nil, "", 0, err
	}
	stream, err := streamConfig(p, host, "tls")
	if err != nil {
		return nil, nil, "", 0, err
	}
	account := &trojan.Account{Password: u.User.Username()}
	out := &trojan.ClientConfig{Server: endpoint(host, port, serial.ToTypedMessage(account))}
	return serial.ToTypedMessage(out), stream, host, port, nil
}

// vmess://<base64 of the v2rayN JSON>
func parseVmess(link string) (*serial.TypedMessage, *internet.StreamConfig, string, int, error) {
	raw, err := decodeB64(link[len("vmess://"):])
	if err != nil {
		return nil, nil, "", 0, fmt.Errorf("vmess:// 链接格式不正确")
	}
	var v map[string]any
	if err := json.Unmarshal(raw, &v); err != nil {
		return nil, nil, "", 0, fmt.Errorf("vmess:// 链接格式不正确")
	}
	str := func(k string) string {
		switch x := v[k].(type) {
		case string:
			return strings.TrimSpace(x)
		case float64:
			return strconv.FormatFloat(x, 'f', -1, 64)
		}
		return ""
	}
	host := strings.Trim(str("add"), "[]")
	port, err := strconv.Atoi(str("port"))
	if host == "" || err != nil || port <= 0 || port > 65535 || str("id") == "" {
		return nil, nil, "", 0, fmt.Errorf("服务器地址或端口不正确")
	}
	if aid := str("aid"); aid != "" && aid != "0" {
		return nil, nil, "", 0, fmt.Errorf("不支持 VMess alterId %s（只支持 0）", aid)
	}
	security := "none"
	if t := str("tls"); t == "tls" || t == "reality" {
		security = t
	}
	p := params{"type": str("net"), "security": security, "sni": str("sni"), "alpn": str("alpn"), "fp": str("fp"),
		"host": str("host"), "path": str("path"), "pbk": str("pbk"), "sid": str("sid"), "spx": str("spx")}
	switch p["type"] {
	case "tcp", "raw", "":
		p["headerType"] = str("type")
	case "grpc":
		p["serviceName"] = str("path")
		p["mode"] = str("type")
	case "xhttp", "splithttp":
		p["mode"] = str("type")
	}
	stream, err := streamConfig(p, host, "none")
	if err != nil {
		return nil, nil, "", 0, err
	}
	sec := &protocol.SecurityConfig{Type: protocol.SecurityType_AUTO}
	switch str("scy") {
	case "", "auto":
	case "aes-128-gcm":
		sec.Type = protocol.SecurityType_AES128_GCM
	case "chacha20-poly1305":
		sec.Type = protocol.SecurityType_CHACHA20_POLY1305
	default:
		return nil, nil, "", 0, fmt.Errorf("不支持 VMess 加密方式 %q", str("scy"))
	}
	account := &vmess.Account{Id: str("id"), SecuritySettings: sec}
	out := &vmessout.Config{Receiver: endpoint(host, port, serial.ToTypedMessage(account))}
	return serial.ToTypedMessage(out), stream, host, port, nil
}

// ss://BASE64(method:password)@host:port#name (SIP002, also with a plain
// "method:password"), or the old ss://BASE64(method:password@host:port)#name.
func parseSS(link string) (*serial.TypedMessage, *internet.StreamConfig, string, int, error) {
	rest := link[len("ss://"):]
	if i := strings.IndexByte(rest, '#'); i >= 0 {
		rest = rest[:i]
	}
	if !strings.Contains(rest, "@") {
		dec, err := decodeB64(strings.SplitN(rest, "?", 2)[0])
		if err != nil {
			return nil, nil, "", 0, fmt.Errorf("ss:// 链接格式不正确")
		}
		rest = string(dec)
	}
	at := strings.LastIndexByte(rest, '@')
	if at < 0 {
		return nil, nil, "", 0, fmt.Errorf("ss:// 链接格式不正确")
	}
	userinfo := rest[:at]
	if dec, err := decodeB64(userinfo); err == nil && strings.Contains(string(dec), ":") {
		userinfo = string(dec)
	} else if un, err := url.PathUnescape(userinfo); err == nil {
		userinfo = un
	}
	method, password, ok := strings.Cut(userinfo, ":")
	if !ok || password == "" {
		return nil, nil, "", 0, fmt.Errorf("ss:// 链接格式不正确")
	}
	u, err := url.Parse("ss://x@" + rest[at+1:])
	if err != nil {
		return nil, nil, "", 0, fmt.Errorf("ss:// 链接格式不正确")
	}
	host, port, err := hostPort(u)
	if err != nil {
		return nil, nil, "", 0, err
	}
	query, err := queryParams(u.RawQuery)
	if err != nil {
		return nil, nil, "", 0, err
	}
	if query.get("plugin") != "" {
		return nil, nil, "", 0, fmt.Errorf("不支持 Shadowsocks 插件（plugin）")
	}
	stream := &internet.StreamConfig{ProtocolName: "tcp"}
	method = strings.ToLower(method)
	if strings.HasPrefix(method, "2022-") {
		switch method {
		case "2022-blake3-aes-128-gcm", "2022-blake3-aes-256-gcm", "2022-blake3-chacha20-poly1305":
		default:
			return nil, nil, "", 0, fmt.Errorf("不支持 Shadowsocks 加密方式 %q", method)
		}
		out := &ss2022.ClientConfig{Address: xnet.NewIPOrDomain(xnet.ParseAddress(host)), Port: uint32(port), Method: method, Key: password}
		return serial.ToTypedMessage(out), stream, host, port, nil
	}
	ciphers := map[string]shadowsocks.CipherType{
		"aes-128-gcm":             shadowsocks.CipherType_AES_128_GCM,
		"aes-256-gcm":             shadowsocks.CipherType_AES_256_GCM,
		"chacha20-ietf-poly1305":  shadowsocks.CipherType_CHACHA20_POLY1305,
		"chacha20-poly1305":       shadowsocks.CipherType_CHACHA20_POLY1305,
		"xchacha20-ietf-poly1305": shadowsocks.CipherType_XCHACHA20_POLY1305,
		"xchacha20-poly1305":      shadowsocks.CipherType_XCHACHA20_POLY1305,
	}
	ct, ok := ciphers[method]
	if !ok {
		return nil, nil, "", 0, fmt.Errorf("不支持 Shadowsocks 加密方式 %q", method)
	}
	account := &shadowsocks.Account{Password: password, CipherType: ct}
	out := &shadowsocks.ClientConfig{Server: endpoint(host, port, serial.ToTypedMessage(account))}
	return serial.ToTypedMessage(out), stream, host, port, nil
}

// streamConfig builds the transport and security of a link. defSecurity applies
// when the link names none (trojan:// means TLS).
func streamConfig(p params, server, defSecurity string) (*internet.StreamConfig, error) {
	sc := &internet.StreamConfig{}
	hostHeader := p.get("host")
	path := p.get("path")
	if path == "" || path[0] != '/' {
		path = "/" + path
	}
	setTransport := func(name string, cfg *serial.TypedMessage) {
		sc.ProtocolName = name
		sc.TransportSettings = []*internet.TransportConfig{{ProtocolName: name, Settings: cfg}}
	}
	switch t := strings.ToLower(p.get("type")); t {
	case "", "tcp", "raw":
		if h := p.get("headerType"); h != "" && h != "none" {
			return nil, fmt.Errorf("不支持 TCP 伪装类型 %q", h)
		}
		sc.ProtocolName = "tcp"
	case "ws", "websocket":
		ws := &websocket.Config{Host: hostHeader, Path: path}
		// "/path?ed=2048": early data, as Xray's own config reader handles it.
		if base, q, ok := strings.Cut(path, "?"); ok {
			if v, err := url.ParseQuery(q); err == nil {
				if ed, err := strconv.Atoi(v.Get("ed")); err == nil && ed > 0 {
					ws.Ed = uint32(ed)
					v.Del("ed")
					ws.Path = base
					if len(v) > 0 {
						ws.Path += "?" + v.Encode()
					}
				}
			}
		}
		setTransport("websocket", serial.ToTypedMessage(ws))
	case "grpc":
		serviceName := p.get("serviceName")
		if serviceName == "" {
			serviceName = p.get("servicename")
		}
		if serviceName == "" {
			serviceName = "Tun"
		}
		setTransport("grpc", serial.ToTypedMessage(&grpcTransport.Config{
			ServiceName: serviceName,
			Authority:   p.get("authority"),
		}))
	case "httpupgrade":
		setTransport("httpupgrade", serial.ToTypedMessage(&httpupgrade.Config{Host: hostHeader, Path: path}))
	case "xhttp", "splithttp":
		mode := p.get("mode")
		if mode == "" {
			mode = "auto"
		}
		setTransport("splithttp", serial.ToTypedMessage(&splithttp.Config{Host: hostHeader, Path: path, Mode: mode}))
	default:
		return nil, fmt.Errorf("不支持传输方式 %q", t)
	}

	security := strings.ToLower(p.get("security"))
	if security == "" {
		security = defSecurity
	}
	fp := p.get("fp")
	sni := p.get("sni")
	if sni == "" {
		sni = p.get("peer")
	}
	switch security {
	case "none":
	case "tls":
		if v, v2 := strings.ToLower(p.get("allowInsecure")), strings.ToLower(p.get("insecure")); v == "1" || v == "true" || v2 == "1" || v2 == "true" {
			return nil, fmt.Errorf("不支持跳过证书验证（allowInsecure / insecure）的链接")
		}
		if sni == "" {
			sni = hostHeader
		}
		if sni == "" && net.ParseIP(server) == nil {
			sni = server
		}
		tc := &tls.Config{ServerName: sni, Fingerprint: fp}
		if alpn := p.get("alpn"); alpn != "" {
			tc.NextProtocol = strings.Split(alpn, ",")
		}
		sc.SecurityType = serial.GetMessageType(tc)
		sc.SecuritySettings = []*serial.TypedMessage{serial.ToTypedMessage(tc)}
	case "reality":
		pbk, err := base64.RawURLEncoding.DecodeString(p.get("pbk"))
		if err != nil || len(pbk) != 32 {
			return nil, fmt.Errorf("REALITY 公钥（pbk）不正确")
		}
		sid := make([]byte, 8)
		if s := p.get("sid"); len(s) > 16 {
			return nil, fmt.Errorf("REALITY short id（sid）不正确")
		} else if _, err := hex.Decode(sid, []byte(s)); err != nil {
			return nil, fmt.Errorf("REALITY short id（sid）不正确")
		}
		if fp == "" {
			fp = "chrome"
		}
		spx := p.get("spx")
		if spx == "" || spx[0] != '/' {
			spx = "/"
		}
		rc := &reality.Config{Fingerprint: fp, ServerName: sni, PublicKey: pbk, ShortId: sid, SpiderX: spx, SpiderY: make([]int64, 10)}
		sc.SecurityType = serial.GetMessageType(rc)
		sc.SecuritySettings = []*serial.TypedMessage{serial.ToTypedMessage(rc)}
	default:
		return nil, fmt.Errorf("不支持安全类型 %q", security)
	}
	return sc, nil
}

// decodeB64 accepts standard or URL-safe base64, with or without padding.
func decodeB64(s string) ([]byte, error) {
	s = strings.TrimSpace(s)
	s = strings.TrimRight(s, "=")
	if b, err := base64.RawStdEncoding.DecodeString(s); err == nil {
		return b, nil
	}
	return base64.RawURLEncoding.DecodeString(s)
}
