package main

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/xtls/xray-core/app/dispatcher"
	applog "github.com/xtls/xray-core/app/log"
	"github.com/xtls/xray-core/app/proxyman"
	_ "github.com/xtls/xray-core/app/proxyman/outbound"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/common/protocol"
	"github.com/xtls/xray-core/common/serial"
	"github.com/xtls/xray-core/core"
	"github.com/xtls/xray-core/proxy/vless"
	vlessout "github.com/xtls/xray-core/proxy/vless/outbound"
	"github.com/xtls/xray-core/transport/internet"
	"github.com/xtls/xray-core/transport/internet/reality"
	_ "github.com/xtls/xray-core/transport/internet/tcp"
)

// Line is a private VLESS + REALITY server (a "vless://" share link). Every
// connection, including name resolution, goes through it; there is no direct path.
type Line struct {
	inst   *core.Instance
	server string
	up     atomic.Bool
}

// parseLink reads vless://uuid@host:port?security=reality&sni=..&pbk=..&sid=..&fp=..&flow=..
func parseLink(link string) (*core.Config, string, error) {
	u, err := url.Parse(strings.TrimSpace(link))
	if err != nil || u.Scheme != "vless" || u.User == nil {
		return nil, "", fmt.Errorf("not a vless:// link")
	}
	q := u.Query()
	if q.Get("security") != "reality" {
		return nil, "", fmt.Errorf("only REALITY links are supported")
	}
	if t := q.Get("type"); t != "" && t != "tcp" && t != "raw" {
		return nil, "", fmt.Errorf("unsupported transport %q", t)
	}
	port, err := strconv.Atoi(u.Port())
	if err != nil || port <= 0 || port > 65535 {
		return nil, "", fmt.Errorf("bad port")
	}
	flow := q.Get("flow")
	if flow != "" && flow != vless.XRV {
		return nil, "", fmt.Errorf("unsupported flow %q", flow)
	}
	pbk, err := base64.RawURLEncoding.DecodeString(q.Get("pbk"))
	if err != nil || len(pbk) != 32 {
		return nil, "", fmt.Errorf("bad public key")
	}
	sid := make([]byte, 8)
	if s := q.Get("sid"); len(s) > 16 {
		return nil, "", fmt.Errorf("bad short id")
	} else if _, err := hex.Decode(sid, []byte(s)); err != nil {
		return nil, "", fmt.Errorf("bad short id")
	}
	fp := q.Get("fp")
	if fp == "" {
		fp = "chrome"
	}
	spx := q.Get("spx")
	if spx == "" || spx[0] != '/' {
		spx = "/"
	}

	rc := &reality.Config{
		Fingerprint: fp,
		ServerName:  q.Get("sni"),
		PublicKey:   pbk,
		ShortId:     sid,
		SpiderX:     spx,
		SpiderY:     make([]int64, 10),
	}
	account := &vless.Account{Id: u.User.Username(), Flow: flow, Encryption: "none"}
	out := &vlessout.Config{Vnext: &protocol.ServerEndpoint{
		Address: xnet.NewIPOrDomain(xnet.ParseAddress(u.Hostname())),
		Port:    uint32(port),
		User:    &protocol.User{Account: serial.ToTypedMessage(account)},
	}}
	cfg := &core.Config{
		App: []*serial.TypedMessage{
			// No logs: they would name every site visited.
			serial.ToTypedMessage(&applog.Config{ErrorLogType: applog.LogType_None, AccessLogType: applog.LogType_None}),
			serial.ToTypedMessage(&dispatcher.Config{}),
			serial.ToTypedMessage(&proxyman.OutboundConfig{}),
		},
		Outbound: []*core.OutboundHandlerConfig{{
			SenderSettings: serial.ToTypedMessage(&proxyman.SenderConfig{
				StreamSettings: &internet.StreamConfig{
					ProtocolName:     "tcp",
					SecurityType:     serial.GetMessageType(rc),
					SecuritySettings: []*serial.TypedMessage{serial.ToTypedMessage(rc)},
				},
			}),
			ProxySettings: serial.ToTypedMessage(out),
		}},
	}
	return cfg, net.JoinHostPort(u.Hostname(), u.Port()), nil
}

func StartLine(link string) (*Line, string, error) {
	cfg, server, err := parseLink(link)
	if err != nil {
		return nil, "", err
	}
	inst, err := core.New(cfg)
	if err != nil {
		return nil, "", err
	}
	if err := inst.Start(); err != nil {
		return nil, "", err
	}
	return &Line{inst: inst, server: server}, server, nil
}

func (l *Line) Dial(ctx context.Context, host string, port int) (net.Conn, error) {
	return core.Dial(ctx, l.inst, xnet.TCPDestination(xnet.ParseAddress(host), xnet.Port(port)))
}

// check fetches a 204 page through the line; core.Dial alone succeeds without
// reaching the server.
func (l *Line) check() error {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	c, err := l.Dial(ctx, "cp.cloudflare.com", 80)
	if err != nil {
		return err
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(15 * time.Second))
	if _, err := c.Write([]byte("GET /generate_204 HTTP/1.1\r\nHost: cp.cloudflare.com\r\nConnection: close\r\n\r\n")); err != nil {
		return err
	}
	resp, err := http.ReadResponse(bufio.NewReader(c), nil)
	if err != nil {
		return err
	}
	resp.Body.Close()
	if resp.StatusCode != 204 {
		return fmt.Errorf("check returned %d", resp.StatusCode)
	}
	return nil
}

// reason says why the line is down: "noipv6" when this computer has no IPv6
// route to an IPv6 server, "unreachable" when the server does not answer, or
// "handshake" when it answers but the tunnel does not come up.
func (l *Line) reason() string {
	if c, err := net.DialTimeout("tcp", l.server, 8*time.Second); err == nil {
		c.Close()
		return "handshake"
	}
	host, _, _ := net.SplitHostPort(l.server)
	if ip := net.ParseIP(host); ip != nil && ip.To4() == nil && !routesIPv6(ip) {
		return "noipv6"
	}
	return "unreachable"
}

// routesIPv6 reports whether the system routes to ip from a global IPv6 address.
// Connecting a UDP socket sends nothing; Teredo (2001::/32) does not count.
func routesIPv6(ip net.IP) bool {
	c, err := net.DialUDP("udp6", nil, &net.UDPAddr{IP: ip, Port: 443})
	if err != nil {
		return false
	}
	defer c.Close()
	local := c.LocalAddr().(*net.UDPAddr).IP
	_, teredo, _ := net.ParseCIDR("2001::/32")
	return local.IsGlobalUnicast() && !teredo.Contains(local)
}

// Monitor reports STATE up/down; it checks every 20 s while down, 2 min while up.
func (l *Line) Monitor() {
	first := true
	for {
		err := l.check()
		if err == nil != l.up.Load() || first {
			l.up.Store(err == nil)
			if err == nil {
				status("STATE up")
			} else {
				status("STATE down")
				status("ERROR line: %v", err)
				status("REASON %s", l.reason())
			}
			first = false
		}
		if l.up.Load() {
			time.Sleep(2 * time.Minute)
		} else {
			time.Sleep(20 * time.Second)
		}
	}
}
