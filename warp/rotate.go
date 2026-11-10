package main

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"net"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// Rotating line address.
//
// A line server with a routed IPv6 /64 can answer on a different address every
// RotatePeriod. Server and browser derive the same address from the line's UUID
// and the period number, so nothing has to be looked up or published:
//
//	period  = unix seconds / 259200            (3 days)
//	suffix  = first 8 bytes of SHA-256("ulb-line-v1|" + uuid + "|" + period)
//	address = prefix (first 64 bits) + suffix
//
// server/ulb-line-ipv6.sh does the same on the server. The browser tries this
// period's address, then the previous period's (clock skew, a server that has not
// switched yet), then the address written in the link, so a server that does not
// rotate keeps working.
const RotatePeriod = 3 * 24 * time.Hour

func rotatePeriod(t time.Time) int64 { return t.Unix() / int64(RotatePeriod/time.Second) }

// rotatedAddr is the line address for one period.
func rotatedAddr(prefix netip.Prefix, uuid string, period int64) netip.Addr {
	sum := sha256.Sum256([]byte(fmt.Sprintf("ulb-line-v1|%s|%d", strings.ToLower(uuid), period)))
	b := prefix.Masked().Addr().As16()
	copy(b[8:], sum[:8])
	if binary.BigEndian.Uint64(b[8:]) == 0 {
		b[15] = 1 // never the subnet-router anycast address
	}
	return netip.AddrFrom16(b)
}

// lineCandidates lists the links to try, best first. With an empty or invalid
// prefix it is just the link itself.
func lineCandidates(link, prefix string, now time.Time) []string {
	p, err := netip.ParsePrefix(strings.TrimSpace(prefix))
	if err != nil || !p.Addr().Is6() || p.Bits() > 64 {
		return []string{link}
	}
	u, err := url.Parse(strings.TrimSpace(link))
	if err != nil || u.User == nil || u.Port() == "" {
		return []string{link}
	}
	n := rotatePeriod(now)
	var out []string
	seen := map[string]bool{}
	add := func(l string) {
		if !seen[l] {
			seen[l] = true
			out = append(out, l)
		}
	}
	for _, period := range []int64{n, n - 1} {
		v := *u
		v.Host = net.JoinHostPort(rotatedAddr(p, u.User.Username(), period).String(), u.Port())
		add(v.String())
	}
	add(link)
	return out
}

// hostPortOf is "host:port" of a link, for status lines.
func hostPortOf(link string) string {
	u, err := url.Parse(link)
	if err != nil {
		return ""
	}
	if _, err := strconv.Atoi(u.Port()); err != nil {
		return ""
	}
	return net.JoinHostPort(u.Hostname(), u.Port())
}
