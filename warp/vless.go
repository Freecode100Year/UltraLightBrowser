package main

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	_ "github.com/xtls/xray-core/app/proxyman/outbound"
	xnet "github.com/xtls/xray-core/common/net"
	"github.com/xtls/xray-core/core"
	_ "github.com/xtls/xray-core/transport/internet/tcp"
)

// Line is a private server given as a share link (see links.go). Every
// connection, including name resolution, goes through it; there is no direct path.
//
// With a rotation prefix (see rotate.go) the line has several candidate links,
// best first; the one in use is replaced when it stops working, and the line
// moves back to the best candidate when a new rotation period starts.
type Line struct {
	mu     sync.Mutex
	inst   lineEngine
	server string
	link   string // the link as given
	prefix string // rotation prefix, or ""
	cands  []string
	cur    string // candidate in use
	auto   bool   // cands are independent servers: use the fastest that works
	up     atomic.Bool
}

type lineEngine interface {
	Dial(context.Context, string, int) (net.Conn, error)
	Close() error
}

type xrayLineEngine struct{ inst *core.Instance }

func (e *xrayLineEngine) Dial(ctx context.Context, host string, port int) (net.Conn, error) {
	return core.Dial(ctx, e.inst, xnet.TCPDestination(xnet.ParseAddress(host), xnet.Port(port)))
}
func (e *xrayLineEngine) Close() error { return e.inst.Close() }

// StartLine starts the line. prefix is an IPv6 rotation prefix or "".
func StartLine(link, prefix string) (*Line, string, error) {
	if _, _, err := parseLink(link); err != nil {
		return nil, "", err
	}
	l := &Line{link: link, prefix: prefix, cands: lineCandidates(link, prefix, time.Now())}
	inst, server, err := startInst(l.cands[0])
	if err != nil {
		return nil, "", err
	}
	l.inst, l.server, l.cur = inst, server, l.cands[0]
	return l, server, nil
}

// StartAuto starts a line over several servers, tried at the same time on the
// first check; the fastest that works is used. Links that cannot be used are dropped.
func StartAuto(links []string) (*Line, string, error) {
	var ok []string
	for _, l := range links {
		if _, _, err := parseLink(l); err == nil {
			ok = append(ok, l)
		}
	}
	if len(ok) == 0 {
		return nil, "", fmt.Errorf("没有可用的链接")
	}
	l := &Line{link: ok[0], cands: ok, auto: true}
	inst, server, err := startInst(ok[0])
	if err != nil {
		return nil, "", err
	}
	l.inst, l.server, l.cur = inst, server, ok[0]
	return l, server, nil
}

func startInst(link string) (lineEngine, string, error) {
	cfg, server, err := lineConfigForStart(link)
	if err != nil {
		return nil, "", err
	}
	inst, err := core.New(cfg)
	if err != nil {
		return nil, "", err
	}
	if err := inst.Start(); err != nil {
		inst.Close()
		return nil, "", err
	}
	return &xrayLineEngine{inst: inst}, server, nil
}

func (l *Line) Dial(ctx context.Context, host string, port int) (net.Conn, error) {
	l.mu.Lock()
	inst := l.inst
	l.mu.Unlock()
	return inst.Dial(ctx, host, port)
}

// retire closes an instance that is no longer in use, after a grace period so
// connections already open through it can finish.
func retire(inst lineEngine) {
	go func() {
		time.Sleep(2 * time.Minute)
		inst.Close()
	}()
}

// pick checks the given candidates at the same time (at most ~15 s in all) and
// switches to the first one in list order that works. The instance in use is
// reused for its own candidate. It returns false, changing nothing, when none works.
func (l *Line) pick(cands []string) bool {
	l.mu.Lock()
	curInst, cur := l.inst, l.cur
	l.mu.Unlock()
	type trial struct {
		inst   lineEngine
		server string
		ok     bool
		took   time.Duration
	}
	trials := make([]trial, len(cands))
	var wg sync.WaitGroup
	for i, c := range cands {
		wg.Add(1)
		go func(i int, c string) {
			defer wg.Done()
			var inst lineEngine
			var server string
			if c == cur {
				inst = curInst
			} else {
				var err error
				if inst, server, err = startInst(c); err != nil {
					return
				}
			}
			start := time.Now()
			ok := checkInst(inst) == nil
			trials[i] = trial{inst, server, ok, time.Since(start)}
		}(i, c)
	}
	wg.Wait()
	chosen := -1
	for i, t := range trials {
		if !t.ok {
			continue
		}
		if chosen < 0 {
			chosen = i
			if !l.auto {
				break // rotation candidates: the first in list order wins
			}
		} else if t.took < trials[chosen].took {
			chosen = i
		}
	}
	for i, t := range trials {
		if i != chosen && t.inst != nil && t.inst != curInst {
			t.inst.Close()
		}
	}
	if chosen < 0 {
		return false
	}
	if cands[chosen] == cur {
		return true
	}
	t := trials[chosen]
	l.mu.Lock()
	l.inst, l.server, l.cur = t.inst, t.server, cands[chosen]
	l.mu.Unlock()
	retire(curInst)
	status("LINE %s", t.server)
	return true
}

// check fetches a 204 page through the line; core.Dial alone succeeds without
// reaching the server.
func (l *Line) check() error {
	l.mu.Lock()
	inst := l.inst
	l.mu.Unlock()
	return checkInst(inst)
}

func checkInst(inst lineEngine) error {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	c, err := inst.Dial(ctx, "cp.cloudflare.com", 80)
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
	l.mu.Lock()
	server := l.server
	l.mu.Unlock()
	if c, err := net.DialTimeout("tcp", server, 8*time.Second); err == nil {
		c.Close()
		return "handshake"
	}
	host, _, _ := net.SplitHostPort(server)
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
// With rotation it also fails over between candidates and, while up on a fallback,
// tries the best candidate again when a new period starts and once an hour.
func (l *Line) Monitor() {
	first := true
	lastBest := time.Now()
	for {
		if c := lineCandidates(l.link, l.prefix, time.Now()); !l.auto && !sameList(c, l.candidates()) {
			l.mu.Lock()
			l.cands = c
			l.mu.Unlock()
			lastBest = time.Time{} // new period: move to its address now
		}
		var err error
		if first && len(l.candidates()) > 1 {
			// Start-up: check every candidate at once rather than one after another.
			if !l.pick(l.candidates()) {
				err = l.check()
			}
		} else if err = l.check(); err != nil && len(l.candidates()) > 1 && l.pick(l.candidates()) {
			err = nil
		}
		if err == nil && time.Since(lastBest) > time.Hour {
			if cands := l.candidates(); l.auto && len(cands) > 1 {
				// Servers get slower or faster: look for the fastest again once an hour.
				lastBest = time.Now()
				l.pick(cands)
			} else if l.current() != cands[0] {
				lastBest = time.Now()
				l.pick(cands[:1])
			}
		}
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

func (l *Line) candidates() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.cands
}

func (l *Line) current() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.cur
}

func sameList(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
