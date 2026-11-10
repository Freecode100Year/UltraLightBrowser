package main

import (
	"os"
	"testing"
)

// TestRegisterProfilesLive registers one WARP device with each app profile.
// It talks to Cloudflare, so it only runs with ULB_LIVE=1:
//
//	ULB_LIVE=1 go test -run TestRegisterProfilesLive -v .
func TestRegisterProfilesLive(t *testing.T) {
	if os.Getenv("ULB_LIVE") != "1" {
		t.Skip("set ULB_LIVE=1 to register real WARP devices")
	}
	for _, p := range regProfiles {
		a, err := registerWith(p)
		if err != nil {
			t.Errorf("%s: %v", p.clientVersion, err)
			continue
		}
		if a.PeerKey == "" || a.AddrV6 == "" || len(a.Ports) == 0 {
			t.Errorf("%s: incomplete account %+v", p.clientVersion, a)
			continue
		}
		t.Logf("%s: ok, v6 %s, endpoint %s/%s, %d ports", p.clientVersion, a.AddrV6, a.EndpointV4, a.EndpointV6, len(a.Ports))
	}
}
