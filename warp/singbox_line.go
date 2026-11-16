package main

import (
	"context"
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"

	"github.com/gofrs/uuid/v5"
	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/adapter/certificate"
	sbendpoint "github.com/sagernet/sing-box/adapter/endpoint"
	"github.com/sagernet/sing-box/adapter/inbound"
	sboutbound "github.com/sagernet/sing-box/adapter/outbound"
	boxservice "github.com/sagernet/sing-box/adapter/service"
	C "github.com/sagernet/sing-box/constant"
	"github.com/sagernet/sing-box/dns"
	"github.com/sagernet/sing-box/dns/transport/local"
	"github.com/sagernet/sing-box/option"
	"github.com/sagernet/sing-box/protocol/hysteria2"
	"github.com/sagernet/sing-box/protocol/tuic"
	M "github.com/sagernet/sing/common/metadata"
)

type quicLineProfile struct {
	scheme, server, password, sni, obfsPassword, uuid string
	port                                              uint16
	alpn                                              []string
	congestion, relay                                 string
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
		if u.User == nil {
			return nil, fmt.Errorf("TUIC 链接缺少 UUID 和密码")
		}
		p.uuid = u.User.Username()
		p.password, _ = u.User.Password()
		if p.uuid == "" || p.password == "" {
			return nil, fmt.Errorf("TUIC 链接缺少 UUID 或密码")
		}
		if _, err := uuid.FromString(p.uuid); err != nil {
			return nil, fmt.Errorf("TUIC UUID 格式不正确")
		}
		for _, k := range []string{"alpn", "congestion_control", "udp_relay_mode"} {
			allowed[k] = true
		}
		p.alpn = splitComma(query.Get("alpn"))
		p.congestion = query.Get("congestion_control")
		if p.congestion == "" {
			p.congestion = "cubic"
		}
		if p.congestion != "cubic" && p.congestion != "new_reno" && p.congestion != "bbr" {
			return nil, fmt.Errorf("TUIC 拥塞控制仅支持 cubic、new_reno 或 bbr")
		}
		p.relay = query.Get("udp_relay_mode")
		if p.relay == "" {
			p.relay = "native"
		}
		if p.relay != "native" && p.relay != "quic" {
			return nil, fmt.Errorf("TUIC UDP 转发模式仅支持 native 或 quic")
		}
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

func splitComma(s string) []string {
	var out []string
	for _, v := range strings.Split(s, ",") {
		if v = strings.TrimSpace(v); v != "" {
			out = append(out, v)
		}
	}
	return out
}

type singBoxLineEngine struct {
	instance *box.Box
	outbound adapter.Outbound
}

func (e *singBoxLineEngine) Dial(ctx context.Context, host string, port int) (net.Conn, error) {
	return e.outbound.DialContext(ctx, "tcp", M.ParseSocksaddrHostPort(host, uint16(port)))
}
func (e *singBoxLineEngine) Close() error { return e.instance.Close() }

func startSingBoxLine(p *quicLineProfile) (lineEngine, string, error) {
	var outbound option.Outbound
	tls := &option.OutboundTLSOptions{Enabled: true, ServerName: p.sni, ALPN: p.alpn}
	if p.scheme == "hysteria2" || p.scheme == "hy2" {
		o := &option.Hysteria2OutboundOptions{ServerOptions: option.ServerOptions{Server: p.server, ServerPort: p.port}, Password: p.password, OutboundTLSOptionsContainer: option.OutboundTLSOptionsContainer{TLS: tls}}
		if p.obfsPassword != "" {
			o.Obfs = &option.Hysteria2Obfs{Type: C.Hysteria2ObfsTypeSalamander, Password: p.obfsPassword}
		}
		outbound = option.Outbound{Type: C.TypeHysteria2, Tag: "line", Options: o}
	} else {
		o := &option.TUICOutboundOptions{ServerOptions: option.ServerOptions{Server: p.server, ServerPort: p.port}, UUID: p.uuid, Password: p.password, CongestionControl: p.congestion, UDPRelayMode: p.relay, OutboundTLSOptionsContainer: option.OutboundTLSOptionsContainer{TLS: tls}}
		outbound = option.Outbound{Type: C.TypeTUIC, Tag: "line", Options: o}
	}
	inboundRegistry := inbound.NewRegistry()
	outboundRegistry := sboutbound.NewRegistry()
	hysteria2.RegisterOutbound(outboundRegistry)
	tuic.RegisterOutbound(outboundRegistry)
	endpointRegistry := sbendpoint.NewRegistry()
	dnsRegistry := dns.NewTransportRegistry()
	local.RegisterTransport(dnsRegistry)
	serviceRegistry := boxservice.NewRegistry()
	certificateRegistry := certificate.NewRegistry()
	ctx := box.Context(context.Background(), inboundRegistry, outboundRegistry, endpointRegistry, dnsRegistry, serviceRegistry, certificateRegistry)
	instance, err := box.New(box.Options{Context: ctx, Options: option.Options{
		Log:       &option.LogOptions{Disabled: true},
		Outbounds: []option.Outbound{outbound},
	}})
	if err != nil {
		return nil, "", err
	}
	if err = instance.Start(); err != nil {
		instance.Close()
		return nil, "", err
	}
	proxy, loaded := instance.Outbound().Outbound("line")
	if !loaded {
		instance.Close()
		return nil, "", fmt.Errorf("sing-box 没有创建线路 outbound")
	}
	return &singBoxLineEngine{instance: instance, outbound: proxy}, net.JoinHostPort(p.server, strconv.Itoa(int(p.port))), nil
}

var _ adapter.SimpleLifecycle = (*box.Box)(nil)
