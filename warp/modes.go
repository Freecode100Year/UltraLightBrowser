package main

// One-shot modes used by the settings page; they print their results and exit.
//
//	-probe        test the links in ULB_PROBE (one per line) at the same time:
//	              "PROBE <index> <ms>" or "PROBE <index> fail <reason>".
//	              With -parse-only the links are only checked, not connected:
//	              "PROBE <index> ok".
//	-fetch        download the subscription at ULB_FETCH and print every usable
//	              link in it as "LINK <link>", then "FETCHED <usable> <skipped>".
//	              With -socks host:port it is fetched through that proxy (the
//	              browser's running helper) first, then directly.

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

const maxProbe = 64

func runProbe(parseOnly bool) {
	links := splitLinks(os.Getenv("ULB_PROBE"))
	os.Unsetenv("ULB_PROBE")
	if len(links) > maxProbe {
		links = links[:maxProbe]
	}
	sem := make(chan struct{}, 8)
	var wg sync.WaitGroup
	for i, link := range links {
		if _, _, err := parseLink(link); err != nil {
			status("PROBE %d fail %v", i, err)
			continue
		}
		if parseOnly {
			status("PROBE %d ok", i)
			continue
		}
		wg.Add(1)
		go func(i int, link string) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			ms, err := probeLink(link)
			if err != nil {
				// Xray's own errors ("io: read/write on closed pipe") mean nothing to users.
				status("PROBE %d fail 连不上（服务器没有回应、链接已失效或被封锁）", i)
			} else {
				status("PROBE %d %d", i, ms)
			}
		}(i, link)
	}
	wg.Wait()
}

// probeLink starts a core for link and times one request through it.
func probeLink(link string) (int64, error) {
	inst, _, err := startInst(link)
	if err != nil {
		return 0, err
	}
	defer inst.Close()
	start := time.Now()
	if err := checkInst(inst); err != nil {
		return 0, err
	}
	return time.Since(start).Milliseconds(), nil
}

// splitLinks returns the non-empty lines of s, without surrounding spaces.
func splitLinks(s string) []string {
	var out []string
	for _, l := range strings.Split(strings.ReplaceAll(s, "\r", "\n"), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			out = append(out, l)
		}
	}
	return out
}

const maxSubscription = 4 << 20

func runFetch(socks string) {
	target := strings.TrimSpace(os.Getenv("ULB_FETCH"))
	os.Unsetenv("ULB_FETCH")
	u, err := url.Parse(target)
	if err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "" {
		status("ERROR fetch: 订阅地址要以 http:// 或 https:// 开头")
		os.Exit(1)
	}
	var body []byte
	var lastErr error
	var proxies []*url.URL
	if socks != "" {
		proxies = append(proxies, &url.URL{Scheme: "socks5", Host: socks})
	}
	proxies = append(proxies, nil)
	for _, p := range proxies {
		if body, lastErr = fetchOnce(target, p); lastErr == nil {
			break
		}
	}
	if lastErr != nil {
		status("ERROR fetch: %v", lastErr)
		os.Exit(1)
	}
	links, skipped, err := subscriptionLinks(body)
	if err != nil {
		status("ERROR fetch: %v", err)
		os.Exit(1)
	}
	for _, l := range links {
		status("LINK %s", l)
	}
	status("FETCHED %d %d", len(links), skipped)
}

func fetchOnce(target string, proxy *url.URL) ([]byte, error) {
	tr := &http.Transport{Proxy: func(*http.Request) (*url.URL, error) { return proxy, nil }}
	defer tr.CloseIdleConnections()
	client := &http.Client{Transport: tr, Timeout: 30 * time.Second}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return nil, err
	}
	// Providers return the plain list of share links to v2rayN-style clients.
	req.Header.Set("User-Agent", "v2rayN/7.0")
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("服务器返回 %d", resp.StatusCode)
	}
	return io.ReadAll(io.LimitReader(resp.Body, maxSubscription))
}

// subscriptionLinks reads a subscription: share links one per line, or the
// same list in base64. Lines that are not usable links are counted as skipped.
func subscriptionLinks(body []byte) ([]string, int, error) {
	text := strings.TrimSpace(string(body))
	if !strings.Contains(text, "://") {
		if dec, err := decodeB64(strings.Join(strings.Fields(text), "")); err == nil {
			text = string(dec)
		}
	}
	if strings.Contains(text, "proxies:") && !strings.Contains(text, "://") {
		return nil, 0, fmt.Errorf("不支持 Clash（YAML）格式的订阅，请使用通用 / v2rayN（Base64）格式")
	}
	var links []string
	skipped := 0
	seen := map[string]bool{}
	for _, l := range splitLinks(text) {
		if !strings.Contains(l, "://") {
			continue
		}
		if _, _, err := parseLink(l); err != nil {
			skipped++
			continue
		}
		if !seen[l] {
			seen[l] = true
			links = append(links, l)
		}
	}
	if len(links) == 0 && skipped == 0 {
		return nil, 0, fmt.Errorf("订阅里没有找到分享链接")
	}
	return links, skipped, nil
}
