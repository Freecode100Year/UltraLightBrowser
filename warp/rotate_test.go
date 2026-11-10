package main

import (
	"net/netip"
	"strings"
	"testing"
	"time"
)

// A made-up line (documentation prefix 2001:db8::/32, dummy UUID and key).
// Do not put the built-in public line here: it is kept out of plain text on purpose.
const testLink = "vless://00000000-1111-4222-8333-444444444444@[2001:db8:1:2::10]:443?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.example.com&fp=chrome&pbk=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&sid=01&type=tcp"

// The server script computes the same address with
//
//	printf 'ulb-line-v1|%s|%d' "$UUID" "$PERIOD" | sha256sum | cut -c1-16
//
// This vector was produced that way; if it changes, servers and browsers disagree.
func TestRotatedAddrMatchesServerScript(t *testing.T) {
	p := netip.MustParsePrefix("2001:db8:1:2::/64")
	got := rotatedAddr(p, "00000000-1111-4222-8333-444444444444", 6922)
	want := netip.MustParseAddr("2001:db8:1:2:b7b:8019:b6d2:90a3")
	if got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
	// Upper-case UUIDs give the same address.
	if g := rotatedAddr(p, "00000000-1111-4222-8333-444444444444", 6922); g != want {
		t.Fatalf("case: got %s", g)
	}
}

func TestRotatePeriodIsThreeDays(t *testing.T) {
	start := time.Unix(6922*259200, 0)
	if rotatePeriod(start) != 6922 || rotatePeriod(start.Add(-time.Second)) != 6921 || rotatePeriod(start.Add(RotatePeriod-time.Second)) != 6922 {
		t.Fatal("period boundaries wrong")
	}
}

func TestLineCandidates(t *testing.T) {
	now := time.Unix(6922*259200+100, 0)
	c := lineCandidates(testLink, "2001:db8:1:2::/64", now)
	if len(c) != 3 {
		t.Fatalf("want 3 candidates, got %d: %v", len(c), c)
	}
	if !strings.Contains(c[0], "@[2001:db8:1:2:b7b:8019:b6d2:90a3]:443?") {
		t.Fatalf("first candidate is not this period's address: %s", c[0])
	}
	if c[2] != testLink {
		t.Fatalf("last candidate must be the link itself")
	}
	for _, l := range c {
		if _, _, err := parseLink(l); err != nil {
			t.Fatalf("candidate does not parse: %v\n%s", err, l)
		}
		if !strings.Contains(l, "pbk=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") || !strings.Contains(l, "sni=www.example.com") {
			t.Fatalf("candidate lost parameters: %s", l)
		}
	}
	// Next period: the new address comes first and the old one second.
	n := lineCandidates(testLink, "2001:db8:1:2::/64", now.Add(RotatePeriod))
	if n[1] != c[0] || n[0] == c[0] {
		t.Fatalf("rotation order wrong: %v", n)
	}
}

func TestLineCandidatesWithoutRotation(t *testing.T) {
	for _, prefix := range []string{"", "nonsense", "10.0.0.0/8", "2001:db8::/96"} {
		if c := lineCandidates(testLink, prefix, time.Now()); len(c) != 1 || c[0] != testLink {
			t.Fatalf("prefix %q: want the link only, got %v", prefix, c)
		}
	}
}

func TestLinkWithRotate6StillParses(t *testing.T) {
	l := testLink + "&rotate6=2001:db8:1:2::/64"
	if _, _, err := parseLink(l); err != nil {
		t.Fatalf("rotate6 parameter breaks the link: %v", err)
	}
	if c := lineCandidates(l, "2001:db8:1:2::/64", time.Now()); len(c) != 3 {
		t.Fatalf("want 3 candidates, got %v", c)
	}
}
