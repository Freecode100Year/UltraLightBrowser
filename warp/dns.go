package main

// Android has no /etc/resolv.conf, so Go's own resolver would ask 127.0.0.1:53
// and fail. The Android app passes the phone's DNS servers in ULB_DNS
// ("ip,ip,..."); they are used for the few lookups made outside the tunnel or
// line (WARP registration, a line server given by name). Inside the tunnel and
// over a line, names are resolved remotely as before.

import (
	"context"
	"net"
	"os"
	"strings"
	"sync/atomic"
	"time"
)

// androidCAs points Go at Android's trusted roots: /system/etc/security/cacerts,
// and since Android 14 the updatable set in the Conscrypt APEX. Go reads
// SSL_CERT_DIR lazily, on first use, so setting it at start-up is enough.
func androidCAs() {
	if os.Getenv("SSL_CERT_DIR") != "" {
		return
	}
	var dirs []string
	for _, d := range []string{"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"} {
		if st, err := os.Stat(d); err == nil && st.IsDir() {
			dirs = append(dirs, d)
		}
	}
	if len(dirs) > 0 {
		os.Setenv("SSL_CERT_DIR", strings.Join(dirs, ":"))
	}
}

func setupDNS() {
	androidCAs()
	list := os.Getenv("ULB_DNS")
	os.Unsetenv("ULB_DNS")
	var servers []string
	for _, s := range strings.Split(list, ",") {
		if ip := net.ParseIP(strings.TrimSpace(s)); ip != nil {
			servers = append(servers, net.JoinHostPort(ip.String(), "53"))
		}
	}
	if len(servers) == 0 {
		return
	}
	var next atomic.Uint32
	net.DefaultResolver = &net.Resolver{
		PreferGo: true,
		Dial: func(ctx context.Context, network, _ string) (net.Conn, error) {
			d := net.Dialer{Timeout: 5 * time.Second}
			// Rotate through the servers so one that does not answer is skipped on retry.
			s := servers[int(next.Add(1))%len(servers)]
			return d.DialContext(ctx, network, s)
		},
	}
}
