package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/curve25519"
	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

// Account is the registered WARP device.
type Account struct {
	ID         string `json:"id"`
	Token      string `json:"token"`
	PrivateKey string `json:"private_key"` // base64
	PeerKey    string `json:"peer_public_key"`
	AddrV4     string `json:"address_v4"`
	AddrV6     string `json:"address_v6"`
	EndpointV4 string `json:"endpoint_v4"` // host only
	EndpointV6 string `json:"endpoint_v6"`
	Ports      []int  `json:"ports"`
	// Result of the last endpoint optimisation.
	Best      string    `json:"best_endpoint,omitempty"`
	BestRTTms int       `json:"best_rtt_ms,omitempty"`
	ScannedAt time.Time `json:"scanned_at,omitempty"`
}

type Tunnel struct {
	mu       sync.Mutex
	net      *netstack.Net
	dev      *device.Device
	acct     *Account
	path     string
	endpoint string
	scanning sync.Mutex
}

// Net returns the tunnel's network stack while the tunnel is up.
func (t *Tunnel) Net() *netstack.Net {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.net
}

func (t *Tunnel) setNet(n *netstack.Net) {
	t.mu.Lock()
	was := t.net != nil
	t.net = n
	t.mu.Unlock()
	if was != (n != nil) {
		if n != nil {
			status("STATE up")
		} else {
			status("STATE down")
		}
	}
}

// Resolve looks a host up through Cloudflare DNS inside the tunnel; IPv6 first.
func (t *Tunnel) Resolve(ctx context.Context, host string) ([]net.IP, error) {
	if ip := net.ParseIP(strings.Trim(host, "[]")); ip != nil {
		return []net.IP{ip}, nil
	}
	n := t.Net()
	if n == nil {
		return nil, fmt.Errorf("tunnel down")
	}
	ctx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()
	names, err := n.LookupContextHost(ctx, host)
	if err != nil {
		return nil, err
	}
	var ips []net.IP
	for _, s := range names {
		if ip := net.ParseIP(s); ip != nil {
			ips = append(ips, ip)
		}
	}
	sort.SliceStable(ips, func(i, j int) bool { return ips[i].To4() == nil && ips[j].To4() != nil })
	return ips, nil
}

func (t *Tunnel) Run(statePath string) {
	acct, err := loadAccount(statePath)
	if err != nil {
		acct, err = register()
		for attempt := 1; err != nil; attempt++ {
			status("ERROR register: %v", err)
			time.Sleep(time.Duration(min(attempt*5, 60)) * time.Second)
			acct, err = register()
		}
		saveAccount(statePath, acct)
	}
	t.mu.Lock()
	t.acct, t.path = acct, statePath
	t.mu.Unlock()
	for round := 0; ; round++ {
		if round > 0 {
			// Nothing worked: look for reachable endpoints before trying again.
			t.Optimize()
		}
		t.mu.Lock()
		eps := endpoints(acct)
		t.mu.Unlock()
		for _, ep := range eps {
			if t.connect(acct, ep) {
				t.mu.Lock()
				stale := time.Since(acct.ScannedAt) > 6*time.Hour
				t.mu.Unlock()
				if stale {
					go t.Optimize()
				}
				t.monitor()
			}
		}
		time.Sleep(5 * time.Second)
	}
}

// Optimize scans WARP endpoints and roams the tunnel to the fastest one when it
// is clearly better than the current endpoint.
func (t *Tunnel) Optimize() {
	if !t.scanning.TryLock() {
		return
	}
	defer t.scanning.Unlock()
	t.mu.Lock()
	acct, current := t.acct, t.endpoint
	t.mu.Unlock()
	if acct == nil {
		return
	}
	status("SCAN start")
	results := scan(acct, 160, hasIPv6(), current)
	if len(results) == 0 {
		status("SCAN none")
		return
	}
	best := results[0]
	currentRTT := time.Duration(0)
	for _, r := range results {
		if r.Endpoint == current {
			currentRTT = r.RTT
		}
	}
	t.mu.Lock()
	acct.ScannedAt = time.Now()
	t.mu.Unlock()
	// Switch when the current endpoint no longer answers or another is clearly faster.
	switchTo := current == "" || currentRTT == 0 || best.RTT+5*time.Millisecond < currentRTT*8/10
	if switchTo && best.Endpoint != current {
		t.roam(best.Endpoint)
		current, currentRTT = best.Endpoint, best.RTT
	}
	t.mu.Lock()
	if currentRTT > 0 {
		acct.Best, acct.BestRTTms = current, int(currentRTT.Milliseconds())
	}
	snapshot := *acct
	t.mu.Unlock()
	saveAccount(t.path, &snapshot)
	status("ENDPOINT %s %d", current, currentRTT.Milliseconds())
}

// roam points the running tunnel at another endpoint; open connections survive.
func (t *Tunnel) roam(endpoint string) {
	t.mu.Lock()
	dev, acct := t.dev, t.acct
	t.mu.Unlock()
	if dev == nil || acct == nil {
		return
	}
	// Re-adding the peer drops its old session, so the first packet starts a fresh
	// handshake with the new endpoint at once instead of after a 5 s rekey timeout.
	// Connections live in the netstack and are not affected.
	peer, _ := base64.StdEncoding.DecodeString(acct.PeerKey)
	key := hex.EncodeToString(peer)
	cfg := fmt.Sprintf("public_key=%s\nremove=true\npublic_key=%s\nendpoint=%s\nallowed_ip=0.0.0.0/0\nallowed_ip=::/0\npersistent_keepalive_interval=25\n", key, key, endpoint)
	if dev.IpcSet(cfg) == nil {
		t.mu.Lock()
		t.endpoint = endpoint
		t.mu.Unlock()
	}
}

func hasIPv6() bool {
	c, err := net.Dial("udp6", "[2606:4700:4700::1111]:53")
	if err != nil {
		return false
	}
	c.Close()
	return true
}

func endpoints(a *Account) []string {
	ports := a.Ports
	if len(ports) == 0 {
		ports = []int{2408, 500, 1701, 4500}
	}
	var eps []string
	if a.Best != "" {
		eps = append(eps, a.Best)
	}
	for _, p := range ports {
		if a.EndpointV4 != "" {
			eps = append(eps, net.JoinHostPort(a.EndpointV4, strconv.Itoa(p)))
		}
	}
	for _, p := range ports {
		if a.EndpointV6 != "" {
			eps = append(eps, net.JoinHostPort(a.EndpointV6, strconv.Itoa(p)))
		}
	}
	return eps
}

// connect brings the tunnel up through one endpoint and checks that it carries traffic.
func (t *Tunnel) connect(a *Account, endpoint string) bool {
	t.close()
	var local []netip.Addr
	for _, s := range []string{a.AddrV4, a.AddrV6} {
		if ip, err := netip.ParseAddr(s); err == nil {
			local = append(local, ip)
		}
	}
	dns := []netip.Addr{netip.MustParseAddr("2606:4700:4700::1111"), netip.MustParseAddr("1.1.1.1")}
	tunDev, tnet, err := netstack.CreateNetTUN(local, dns, 1280)
	if err != nil {
		status("ERROR tun: %v", err)
		return false
	}
	dev := device.NewDevice(tunDev, conn.NewDefaultBind(), device.NewLogger(device.LogLevelSilent, ""))
	priv, _ := base64.StdEncoding.DecodeString(a.PrivateKey)
	peer, _ := base64.StdEncoding.DecodeString(a.PeerKey)
	cfg := fmt.Sprintf("private_key=%s\npublic_key=%s\nendpoint=%s\nallowed_ip=0.0.0.0/0\nallowed_ip=::/0\npersistent_keepalive_interval=25\n",
		hex.EncodeToString(priv), hex.EncodeToString(peer), endpoint)
	if err := dev.IpcSet(cfg); err != nil {
		status("ERROR config: %v", err)
		dev.Close()
		return false
	}
	dev.Up()
	t.mu.Lock()
	t.dev = dev
	t.endpoint = endpoint
	t.mu.Unlock()
	if !probe(tnet) {
		t.close()
		return false
	}
	t.setNet(tnet)
	status("ENDPOINT %s 0", endpoint)
	return true
}

// probe opens a TCP connection to Cloudflare's resolver through the tunnel.
func probe(n *netstack.Net) bool {
	for _, a := range []string{"[2606:4700:4700::1111]:443", "1.1.1.1:443"} {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		c, err := n.DialContext(ctx, "tcp", a)
		cancel()
		if err == nil {
			c.Close()
			return true
		}
	}
	return false
}

// monitor returns when the tunnel stops carrying traffic.
func (t *Tunnel) monitor() {
	failures := 0
	for {
		time.Sleep(15 * time.Second)
		n := t.Net()
		if n == nil {
			return
		}
		if probe(n) {
			failures = 0
			continue
		}
		if failures++; failures >= 2 {
			t.close()
			return
		}
	}
}

func (t *Tunnel) close() {
	t.setNet(nil)
	t.mu.Lock()
	dev := t.dev
	t.dev = nil
	t.mu.Unlock()
	if dev != nil {
		dev.Close()
	}
}

func loadAccount(path string) (*Account, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var a Account
	if err := json.Unmarshal(b, &a); err != nil || a.PrivateKey == "" || a.PeerKey == "" {
		return nil, fmt.Errorf("invalid account file")
	}
	return &a, nil
}

func saveAccount(path string, a *Account) {
	b, _ := json.MarshalIndent(a, "", "  ")
	os.MkdirAll(filepath.Dir(path), 0o700)
	tmp := path + ".tmp"
	if os.WriteFile(tmp, b, 0o600) == nil {
		os.Rename(tmp, path)
	}
}

// register creates a free WARP device, the same way the WARP apps do.
func register() (*Account, error) {
	var priv [32]byte
	if _, err := rand.Read(priv[:]); err != nil {
		return nil, err
	}
	priv[0] &= 248
	priv[31] = (priv[31] & 127) | 64
	pub, err := curve25519.X25519(priv[:], curve25519.Basepoint)
	if err != nil {
		return nil, err
	}
	body, _ := json.Marshal(map[string]any{
		"install_id": "", "fcm_token": "", "tos": time.Now().UTC().Format("2006-01-02T15:04:05.000Z"),
		"key": base64.StdEncoding.EncodeToString(pub), "type": "Android", "model": "PC", "locale": "en_US", "warp_enabled": true,
	})
	req, _ := http.NewRequest("POST", "https://api.cloudflareclient.com/v0a2158/reg", bytes.NewReader(body))
	req.Header.Set("User-Agent", "okhttp/3.12.1")
	req.Header.Set("CF-Client-Version", "a-6.11-2158")
	req.Header.Set("Content-Type", "application/json")
	client := &http.Client{Timeout: 20 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	var r struct {
		ID     string `json:"id"`
		Token  string `json:"token"`
		Config struct {
			Peers []struct {
				PublicKey string `json:"public_key"`
				Endpoint  struct {
					V4    string `json:"v4"`
					V6    string `json:"v6"`
					Ports []int  `json:"ports"`
				} `json:"endpoint"`
			} `json:"peers"`
			Interface struct {
				Addresses struct {
					V4 string `json:"v4"`
					V6 string `json:"v6"`
				} `json:"addresses"`
			} `json:"interface"`
		} `json:"config"`
	}
	if err := json.Unmarshal(raw, &r); err != nil || len(r.Config.Peers) == 0 {
		return nil, fmt.Errorf("unexpected registration reply")
	}
	p := r.Config.Peers[0]
	hostOnly := func(s string) string {
		if h, _, err := net.SplitHostPort(s); err == nil {
			return h
		}
		return strings.Trim(s, "[]")
	}
	return &Account{
		ID: r.ID, Token: r.Token, PrivateKey: base64.StdEncoding.EncodeToString(priv[:]), PeerKey: p.PublicKey,
		AddrV4: r.Config.Interface.Addresses.V4, AddrV6: r.Config.Interface.Addresses.V6,
		EndpointV4: hostOnly(p.Endpoint.V4), EndpointV6: hostOnly(p.Endpoint.V6), Ports: p.Endpoint.Ports,
	}, nil
}
