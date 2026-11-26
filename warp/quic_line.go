package main

// Hysteria2 lines (hysteria2:// and hy2://), run by Xray's own Hysteria2
// (proxy/hysteria + transport/internet/hysteria over QUIC, Salamander obfuscation
// as a UDP mask). The certificate is always checked; links that skip the check
// are refused like for the other protocols.

import (
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"

	"github.com/xtls/xray-core/app/dispatcher"
	applog "github.com/xtls/xray-core/app/log"
	"github.com/xtls/xray-core/app/proxyman"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/common/protocol"
	"github.com/xtls/xray-core/common/serial"
	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/hysteria"
	"github.com/xtls/xray-core/transport/internet"
	"github.com/xtls/xray-core/transport/internet/finalmask/salamander"
	hytransport "github.com/xtls/xray-core/transport/internet/hysteria"
	"github.com/xtls/xray-core/transport/internet/tls"
)

type quicLineProfile struct {
	scheme, server, password, sni, obfsPassword string
	port                                        uint16
}

func parseQUICLine(raw string) (*quicLineProfile, error) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u.Hostname() == "" {
		return nil, fmt.Errorf("线路地址格式不正确")
	}
	query, err := url.ParseQuery(u.RawQuery)
	if err != nil {
		return nil, fmt.Errorf("线路参数格式不正确")
	}
	scheme := strings.ToLower(u.Scheme)
	if scheme != "hysteria2" && scheme != "hy2" && scheme != "tuic" {
		return nil, fmt.Errorf("不是 Hysteria2 或 TUIC 链接")
	}
	port, err := strconv.Atoi(u.Port())
	if err != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("服务器端口不正确")
	}
	p := &quicLineProfile{scheme: scheme, server: u.Hostname(), port: uint16(port), sni: strings.TrimSpace(query.Get("sni"))}
	allowed := map[string]bool{"sni": true}
	if scheme == "hysteria2" || scheme == "hy2" {
		if u.User == nil {
			return nil, fmt.Errorf("Hysteria2 链接缺少密码")
		}
		p.password = u.User.Username()
		if pass, ok := u.User.Password(); ok {
			p.password = pass
		}
		if p.password == "" {
			return nil, fmt.Errorf("Hysteria2 链接缺少密码")
		}
		allowed["obfs"] = true
		allowed["obfs-password"] = true
		if query.Get("obfs") != "" && query.Get("obfs") != "salamander" {
			return nil, fmt.Errorf("Hysteria2 仅支持 salamander 混淆")
		}
		p.obfsPassword = query.Get("obfs-password")
		if (query.Get("obfs") == "") != (p.obfsPassword == "") {
			return nil, fmt.Errorf("Hysteria2 混淆类型与密码必须同时填写")
		}
	} else {
		// TUIC would need sing-box: ~18 MB more, and on Android it cannot send at
		// all (its interface monitor needs netlink, which apps are refused).
		return nil, fmt.Errorf("暂不支持 TUIC 线路，请改用 Hysteria2、VLESS 或 Trojan")
	}
	for k := range query {
		if !allowed[k] {
			return nil, fmt.Errorf("暂不支持线路参数 %s", k)
		}
		if len(query[k]) != 1 {
			return nil, fmt.Errorf("线路参数 %s 重复填写", k)
		}
	}
	if p.sni == "" {
		p.sni = p.server
	}
	return p, nil
}

// hysteriaConfig is the Xray config for one Hysteria2 line.
func hysteriaConfig(p *quicLineProfile) (*core.Config, string) {
	tc := &tls.Config{ServerName: p.sni, NextProtocol: []string{"h3"}}
	stream := &internet.StreamConfig{
		ProtocolName: "hysteria",
		TransportSettings: []*internet.TransportConfig{{
			ProtocolName: "hysteria",
			Settings:     serial.ToTypedMessage(&hytransport.Config{Auth: p.password, UdpIdleTimeout: 60}),
		}},
		SecurityType:     serial.GetMessageType(tc),
		SecuritySettings: []*serial.TypedMessage{serial.ToTypedMessage(tc)},
	}
	if p.obfsPassword != "" {
		stream.Udpmasks = []*serial.TypedMessage{serial.ToTypedMessage(&salamander.Config{Password: p.obfsPassword})}
	}
	out := &hysteria.ClientConfig{Server: &protocol.ServerEndpoint{
		Address: xnet.NewIPOrDomain(xnet.ParseAddress(p.server)),
		Port:    uint32(p.port),
	}}
	cfg := &core.Config{
		App: []*serial.TypedMessage{
			serial.ToTypedMessage(&applog.Config{ErrorLogType: applog.LogType_None, AccessLogType: applog.LogType_None}),
			serial.ToTypedMessage(&dispatcher.Config{}),
			serial.ToTypedMessage(&proxyman.OutboundConfig{}),
		},
		Outbound: []*core.OutboundHandlerConfig{{
			SenderSettings: serial.ToTypedMessage(&proxyman.SenderConfig{StreamSettings: stream}),
			ProxySettings:  serial.ToTypedMessage(out),
		}},
	}
	return cfg, net.JoinHostPort(p.server, strconv.Itoa(int(p.port)))
}
