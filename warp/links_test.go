package main

import (
	"encoding/base64"
	"strings"
	"testing"
)

const testUUID = "8f4c3a2e-1b2d-4c5e-9f6a-7b8c9d0e1f2a"
const testPBK = "NOs_DK917gn0YXY6BreYoZvhZrOnDP9jUo1G2D6Aui8"

func vmessLink(json string) string {
	return "vmess://" + base64.StdEncoding.EncodeToString([]byte(json))
}

func TestParseLinkAccepts(t *testing.T) {
	ok := map[string]string{
		"vless reality": "vless://" + testUUID + "@[2001:db8::1]:443?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.example.com&fp=chrome&pbk=" + testPBK + "&sid=14702eaec705a52e&type=tcp#a",
		"vless ws tls":  "vless://" + testUUID + "@example.com:443?security=tls&type=ws&path=%2Fws%3Fed%3D2048&host=cdn.example.com",
		"vless xhttp":   "vless://" + testUUID + "@example.com:443?security=tls&type=xhttp&path=%2Fx&mode=packet-up",
		"vless hu":      "vless://" + testUUID + "@192.0.2.1:80?security=none&type=httpupgrade&path=%2Fhu",
		"vless grpc":    "vless://" + testUUID + "@example.com:443?security=tls&type=grpc&serviceName=x",
		"trojan":        "trojan://secret@example.com:443?sni=example.com#t",
		"trojan ws":     "trojan://secret@example.com:443?type=ws&path=%2Ft",
		"vmess ws tls":  vmessLink(`{"v":"2","add":"example.com","port":443,"id":"` + testUUID + `","aid":"0","net":"ws","path":"/v","tls":"tls","sni":"example.com"}`),
		"ss sip002":     "ss://" + base64.RawURLEncoding.EncodeToString([]byte("aes-256-gcm:pw")) + "@192.0.2.1:8388#s",
		"ss plain 2022": "ss://2022-blake3-aes-128-gcm:AAAAAAAAAAAAAAAAAAAAAA%3D%3D@192.0.2.1:8388",
		"ss legacy":     "ss://" + base64.StdEncoding.EncodeToString([]byte("chacha20-ietf-poly1305:pw@192.0.2.1:8388")) + "#old",
		"hysteria2":     "hysteria2://secret@example.com:443?sni=example.com&obfs=salamander&obfs-password=mask#hy",
		"hy2":           "hy2://secret@example.com:8443#hy2",
	}
	for name, link := range ok {
		if _, server, err := parseLink(link); err != nil || server == "" {
			t.Errorf("%s: %v", name, err)
		}
	}
}

func TestParseLinkRejects(t *testing.T) {
	bad := map[string]string{
		"allowInsecure":   "trojan://secret@example.com:443?allowInsecure=1",
		"insecure":        "vless://" + testUUID + "@example.com:443?security=tls&insecure=true",
		"hy2 insecure":    "hysteria2://secret@example.com:443?insecure=1",
		"tuic":            "tuic://" + testUUID + ":secret@example.com:443",
		"plugin":          "ss://" + base64.RawURLEncoding.EncodeToString([]byte("aes-256-gcm:pw")) + "@192.0.2.1:8388?plugin=obfs-local",
		"old cipher":    "ss://" + base64.RawURLEncoding.EncodeToString([]byte("rc4-md5:pw")) + "@192.0.2.1:8388",
		"alterId":       vmessLink(`{"add":"example.com","port":"443","id":"` + testUUID + `","aid":"64"}`),
		"vision on ws":  "vless://" + testUUID + "@example.com:443?security=tls&type=ws&flow=xtls-rprx-vision",
		"reality key":   "vless://" + testUUID + "@example.com:443?security=reality&pbk=short",
		"no port":       "vless://" + testUUID + "@example.com?security=none",
		"http":          "http://example.com/",
		"tcp header":    "vless://" + testUUID + "@example.com:80?type=tcp&headerType=http",
	}
	for name, link := range bad {
		if _, _, err := parseLink(link); err == nil {
			t.Errorf("%s: accepted %q", name, link)
		}
	}
}

func TestSubscriptionLinks(t *testing.T) {
	good := "trojan://secret@example.com:443#1\nss://" + base64.RawURLEncoding.EncodeToString([]byte("aes-128-gcm:pw")) + "@192.0.2.1:8388#2"
	bad := "vless://" + testUUID + "@example.com:80?type=tcp&headerType=http"
	for name, body := range map[string]string{
		"plain":  good + "\n" + bad + "\n\n# comment\n",
		"base64": base64.StdEncoding.EncodeToString([]byte(good + "\r\n" + bad)),
		"wrap":   strings.Join(strings.SplitAfterN(base64.StdEncoding.EncodeToString([]byte(good+"\n"+bad)), "", 3), "\n"),
	} {
		links, skipped, err := subscriptionLinks([]byte(body))
		if err != nil || len(links) != 2 || skipped != 1 {
			t.Errorf("%s: links=%d skipped=%d err=%v", name, len(links), skipped, err)
		}
	}
	if _, _, err := subscriptionLinks([]byte("proxies:\n  - name: a\n")); err == nil {
		t.Error("clash yaml accepted")
	}
}
