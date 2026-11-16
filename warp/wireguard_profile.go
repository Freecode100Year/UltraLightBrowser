package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"time"

	"github.com/xtls/xray-core/app/dispatcher"
	dnsApp "github.com/xtls/xray-core/app/dns"
	applog "github.com/xtls/xray-core/app/log"
	"github.com/xtls/xray-core/app/proxyman"
	"github.com/xtls/xray-core/app/router"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/common/serial"
	"github.com/xtls/xray-core/core"
	wg "github.com/xtls/xray-core/proxy/wireguard"
)

type wireGuardProfile struct {
	privateKey string
	addresses  []string
	dns        []string
	mtu        int32
	publicKey  string
	preShared  string
	endpoint   string
	allowedIPs []string
	keepAlive  string
}

// parseWireGuardLink decodes the app-private wireguard://<base64url> record.
// It is deliberately distinct from a public WireGuard URI scheme.
func parseWireGuardLink(link string) (*core.Config, string, error) {
	p, server, err := parseWireGuardLinkProfile(link)
	if err != nil {
		return nil, "", err
	}
	return buildWireGuardConfig(p), server, nil
}

func parseWireGuardLinkProfile(link string) (*wireGuardProfile, string, error) {
	const prefix = "wireguard://"
	if !strings.HasPrefix(strings.ToLower(link), prefix) {
		return nil, "", fmt.Errorf("WireGuard 配置格式不正确")
	}
	encoded := strings.TrimSpace(link[len(prefix):])
	if encoded == "" || strings.ContainsAny(encoded, "?#&") {
		return nil, "", fmt.Errorf("WireGuard 配置格式不正确")
	}
	data, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil {
		return nil, "", fmt.Errorf("WireGuard 配置编码不正确")
	}
	p, err := parseWireGuardConfig(string(data))
	if err != nil {
		return nil, "", err
	}
	host, port, _ := net.SplitHostPort(p.endpoint)
	return p, net.JoinHostPort(host, port), nil
}

func buildWireGuardConfig(p *wireGuardProfile) *core.Config {
	peers := []*wg.PeerConfig{{
		PublicKey:    p.publicKey,
		PreSharedKey: p.preShared,
		Endpoint:     p.endpoint,
		KeepAlive:    p.keepAlive,
		AllowedIps:   p.allowedIPs,
	}}
	appDNS := &dnsApp.Config{
		Tag:             "ulb-wg-dns",
		QueryStrategy:   dnsApp.QueryStrategy_USE_IP,
		DisableFallback: true,
	}
	for _, ip := range p.dns {
		addr := netip.MustParseAddr(ip)
		appDNS.NameServer = append(appDNS.NameServer, &dnsApp.NameServer{
			Address: &xnet.Endpoint{Address: xnet.NewIPOrDomain(xnet.ParseAddress(addr.String())), Port: 53},
		})
	}
	cfg := &core.Config{
		App: []*serial.TypedMessage{
			serial.ToTypedMessage(&applog.Config{ErrorLogType: applog.LogType_None, AccessLogType: applog.LogType_None}),
			serial.ToTypedMessage(appDNS),
			serial.ToTypedMessage(&router.Config{
				DomainStrategy: router.Config_AsIs,
				Rule: []*router.RoutingRule{{
					TargetTag:  &router.RoutingRule_Tag{Tag: "wireguard"},
					InboundTag: []string{"ulb-wg-dns"},
				}},
			}),
			serial.ToTypedMessage(&dispatcher.Config{}),
			serial.ToTypedMessage(&proxyman.OutboundConfig{}),
		},
		Outbound: []*core.OutboundHandlerConfig{{
			Tag: "wireguard",
			ProxySettings: serial.ToTypedMessage(&wg.DeviceConfig{
				SecretKey:   p.privateKey,
				Endpoint:    p.addresses,
				Peers:       peers,
				Mtu:         p.mtu,
				IsClient:    true,
				NoKernelTun: true,
				DNS:         p.dns,
			}),
		}},
	}
	return cfg
}

// lineConfigForStart resolves only the WireGuard server endpoint with the
// device resolver before tunnel startup. Website DNS stays on the tagged Xray
// DNS path routed through the WireGuard outbound.
func lineConfigForStart(link string) (*core.Config, string, error) {
	if !strings.HasPrefix(strings.ToLower(link), "wireguard://") {
		return parseLink(link)
	}
	p, server, err := parseWireGuardLinkProfile(link)
	if err != nil {
		return nil, "", err
	}
	host, port, err := net.SplitHostPort(p.endpoint)
	if err != nil {
		return nil, "", fmt.Errorf("WireGuard Endpoint 必须是 host:port")
	}
	if net.ParseIP(host) == nil {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		ips, lookupErr := net.DefaultResolver.LookupIPAddr(ctx, host)
		cancel()
		if lookupErr != nil || len(ips) == 0 {
			return nil, "", fmt.Errorf("无法解析 WireGuard 服务器地址")
		}
		p.endpoint = net.JoinHostPort(ips[0].IP.String(), port)
	}
	return buildWireGuardConfig(p), server, nil
}

func parseWireGuardConfig(text string) (*wireGuardProfile, error) {
	if len(text) == 0 || len(text) > 64*1024 {
		return nil, fmt.Errorf("WireGuard 配置为空或超过 64 KB")
	}
	p := &wireGuardProfile{}
	sections := map[string]map[string]string{"interface": {}, "peer": {}}
	section := ""
	peerCount := 0
	interfaceCount := 0
	for lineNo, raw := range strings.Split(strings.ReplaceAll(text, "\r", ""), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if i := strings.Index(line, " #"); i >= 0 {
			line = strings.TrimSpace(line[:i])
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			name := strings.ToLower(strings.TrimSpace(line[1 : len(line)-1]))
			switch name {
			case "interface":
				interfaceCount++
				if interfaceCount > 1 || section == "peer" {
					return nil, fmt.Errorf("WireGuard 配置只能有一个 [Interface]")
				}
				section = "interface"
			case "peer":
				peerCount++
				if peerCount > 1 {
					return nil, fmt.Errorf("目前只支持一个 [Peer] 的 WireGuard 配置")
				}
				section = "peer"
			default:
				return nil, fmt.Errorf("WireGuard 配置第 %d 行包含不支持的区段", lineNo+1)
			}
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok || section == "" {
			return nil, fmt.Errorf("WireGuard 配置第 %d 行格式不正确", lineNo+1)
		}
		key = strings.ToLower(strings.TrimSpace(key))
		value = strings.TrimSpace(value)
		if value == "" {
			return nil, fmt.Errorf("WireGuard 配置第 %d 行缺少值", lineNo+1)
		}
		allowed := map[string]bool{}
		if section == "interface" {
			allowed = map[string]bool{"privatekey": true, "address": true, "dns": true, "mtu": true}
		} else {
			allowed = map[string]bool{"publickey": true, "presharedkey": true, "endpoint": true, "allowedips": true, "persistentkeepalive": true}
		}
		if !allowed[key] {
			return nil, fmt.Errorf("WireGuard 配置第 %d 行的 %s 选项暂不支持", lineNo+1, key)
		}
		if _, exists := sections[section][key]; exists {
			return nil, fmt.Errorf("WireGuard 配置第 %d 行重复定义 %s", lineNo+1, key)
		}
		sections[section][key] = value
	}

	iface, peer := sections["interface"], sections["peer"]
	if interfaceCount != 1 || peerCount != 1 {
		return nil, fmt.Errorf("WireGuard 配置需要一个 [Interface] 和一个 [Peer]")
	}
	p.privateKey = iface["privatekey"]
	p.publicKey = peer["publickey"]
	p.preShared = peer["presharedkey"]
	p.endpoint = peer["endpoint"]
	if p.privateKey == "" || p.publicKey == "" || p.endpoint == "" || peer["allowedips"] == "" {
		return nil, fmt.Errorf("WireGuard 配置缺少 PrivateKey、PublicKey、Endpoint 或 AllowedIPs")
	}
	if len(iface["dns"]) == 0 {
		return nil, fmt.Errorf("WireGuard 配置需要 DNS 地址，网页域名查询必须经隧道转发")
	}
	if err := validWGKey(p.privateKey); err != nil {
		return nil, fmt.Errorf("WireGuard PrivateKey 格式不正确")
	}
	if err := validWGKey(p.publicKey); err != nil {
		return nil, fmt.Errorf("WireGuard PublicKey 格式不正确")
	}
	if p.preShared != "" && validWGKey(p.preShared) != nil {
		return nil, fmt.Errorf("WireGuard PresharedKey 格式不正确")
	}
	for _, raw := range splitWGList(iface["address"]) {
		prefix, err := netip.ParsePrefix(raw)
		if err != nil || !prefix.IsValid() {
			return nil, fmt.Errorf("WireGuard Address 格式不正确")
		}
		p.addresses = append(p.addresses, prefix.String())
	}
	if len(p.addresses) == 0 {
		return nil, fmt.Errorf("WireGuard 配置缺少 Address")
	}
	for _, raw := range splitWGList(iface["dns"]) {
		ip, err := netip.ParseAddr(raw)
		if err != nil || ip.Zone() != "" {
			return nil, fmt.Errorf("WireGuard DNS 必须是 IP 地址")
		}
		p.dns = append(p.dns, ip.String())
	}
	for _, raw := range splitWGList(peer["allowedips"]) {
		prefix, err := netip.ParsePrefix(raw)
		if err != nil || !prefix.IsValid() {
			return nil, fmt.Errorf("WireGuard AllowedIPs 格式不正确")
		}
		p.allowedIPs = append(p.allowedIPs, prefix.String())
	}
	if len(p.allowedIPs) == 0 {
		return nil, fmt.Errorf("WireGuard 配置缺少 AllowedIPs")
	}
	for _, rawDNS := range p.dns {
		dnsIP := netip.MustParseAddr(rawDNS)
		reachable := false
		for _, rawPrefix := range p.allowedIPs {
			prefix := netip.MustParsePrefix(rawPrefix)
			if prefix.Contains(dnsIP) {
				reachable = true
				break
			}
		}
		if !reachable {
			return nil, fmt.Errorf("WireGuard DNS %s 不在 Peer AllowedIPs 范围内，无法确保 DNS 经隧道访问", rawDNS)
		}
	}
	host, portText, err := net.SplitHostPort(p.endpoint)
	port, portErr := strconv.Atoi(portText)
	if err != nil || host == "" || portErr != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("WireGuard Endpoint 必须是 host:port")
	}
	if mtu := iface["mtu"]; mtu != "" {
		n, err := strconv.Atoi(mtu)
		if err != nil || n < 576 || n > 9000 {
			return nil, fmt.Errorf("WireGuard MTU 必须在 576 到 9000 之间")
		}
		p.mtu = int32(n)
	}
	if keep := peer["persistentkeepalive"]; keep != "" && keep != "off" {
		n, err := strconv.Atoi(keep)
		if err != nil || n < 0 || n > 65535 {
			return nil, fmt.Errorf("WireGuard PersistentKeepalive 格式不正确")
		}
		p.keepAlive = strconv.Itoa(n)
	}
	return p, nil
}

func splitWGList(value string) []string {
	var out []string
	for _, part := range strings.Split(value, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}

func validWGKey(value string) error {
	b, err := base64.StdEncoding.DecodeString(value)
	if err != nil {
		b, err = base64.RawStdEncoding.DecodeString(value)
	}
	if err != nil || len(b) != 32 {
		return fmt.Errorf("invalid WireGuard key")
	}
	return nil
}
