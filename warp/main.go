// ulb-warp: Cloudflare WARP for UltraLightBrowser.
//
// Registers a free WARP device (once; the account is kept in -state), brings up a
// userspace WireGuard tunnel (no driver, no administrator rights) and serves a local
// SOCKS5 proxy for the browser. Host names are resolved inside the tunnel by
// Cloudflare DNS and IPv6 is tried first. If the tunnel is down, connections go
// direct unless -fail-closed is given. Status lines go to stdout:
//
//	PORT <n>         proxy is listening
//	STATE up|down    tunnel state changes
//	ENDPOINT <ip:port> <rtt ms>   endpoint in use (after optimisation)
//	SCAN start|end|none
//	ERROR <text>
//
// stdin accepts "RESCAN" (optimise the endpoint now).
//
// With a vless:// link in the ULB_LINE environment variable the helper uses that
// private VLESS + REALITY server instead of WARP (-state is then not needed):
// every connection and name lookup goes through it, never direct. It prints
// PORT, STATE and ERROR lines, plus "LINE <host:port>" once the core is running.
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"
)

var (
	statePath  = flag.String("state", "", "path of the WARP account file")
	listenAddr = flag.String("listen", "127.0.0.1:0", "SOCKS5 listen address")
	parentPID  = flag.Int("parent", 0, "exit when this process exits")
	failClosed = flag.Bool("fail-closed", false, "refuse connections while the tunnel is down")
)

var out sync.Mutex

// line is set when the helper runs a private line instead of WARP.
var line *Line

func status(format string, args ...any) {
	out.Lock()
	defer out.Unlock()
	fmt.Fprintf(os.Stdout, format+"\n", args...)
}

func main() {
	flag.Parse()
	link := os.Getenv("ULB_LINE")
	os.Unsetenv("ULB_LINE")
	if *statePath == "" && link == "" {
		status("ERROR missing -state")
		os.Exit(2)
	}
	if *parentPID != 0 {
		go watchParent(*parentPID)
	}

	ln, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		status("ERROR listen: %v", err)
		os.Exit(1)
	}
	status("PORT %d", ln.Addr().(*net.TCPAddr).Port)

	t := &Tunnel{}
	if link != "" {
		l, server, err := StartLine(link)
		if err != nil {
			status("ERROR line: %v", err)
			os.Exit(1)
		}
		line = l
		status("LINE %s", server)
		go l.Monitor()
	} else {
		go t.Run(*statePath)
		go readCommands(t)
	}

	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		go serveSocks(c, t)
	}
}

// dial connects to host:port, through the tunnel when it is up. A name that does
// not resolve inside the tunnel fails rather than going direct: the direct path
// would ask the system resolver and connect from the real address.
func dial(ctx context.Context, t *Tunnel, host string, port int) (net.Conn, error) {
	if line != nil {
		return line.Dial(ctx, host, port)
	}
	if nt := t.Net(); nt != nil {
		addrs, err := t.Resolve(ctx, host)
		if err != nil {
			return nil, err
		}
		if len(addrs) == 0 {
			return nil, fmt.Errorf("no address for %s", host)
		}
		return dialFirst(ctx, addrs, port, func(ctx context.Context, a string) (net.Conn, error) {
			return nt.DialContext(ctx, "tcp", a)
		})
	}
	if *failClosed {
		return nil, fmt.Errorf("tunnel down")
	}
	d := net.Dialer{Timeout: 10 * time.Second}
	return d.DialContext(ctx, "tcp", net.JoinHostPort(host, fmt.Sprint(port)))
}

type dialResult struct {
	c   net.Conn
	err error
}

// dialFirst tries the addresses in order (IPv6 first), starting the next one
// after 250 ms without an answer (Happy Eyeballs); the first success wins.
func dialFirst(ctx context.Context, addrs []net.IP, port int, dialOne func(context.Context, string) (net.Conn, error)) (net.Conn, error) {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	results := make(chan dialResult, len(addrs))
	pending := 0
	var lastErr error
	win := func(c net.Conn) (net.Conn, error) {
		// Close connections that complete after the winner.
		go func(n int) {
			for ; n > 0; n-- {
				if r := <-results; r.err == nil {
					r.c.Close()
				}
			}
			cancel()
		}(pending)
		return c, nil
	}
	for i, ip := range addrs {
		if i > 0 {
			select {
			case r := <-results:
				pending--
				if r.err == nil {
					return win(r.c)
				}
				lastErr = r.err
			case <-time.After(250 * time.Millisecond):
			}
		}
		pending++
		go func(a string) {
			c, err := dialOne(ctx, a)
			results <- dialResult{c, err}
		}(net.JoinHostPort(ip.String(), fmt.Sprint(port)))
	}
	for pending > 0 {
		r := <-results
		pending--
		if r.err == nil {
			return win(r.c)
		}
		lastErr = r.err
	}
	cancel()
	return nil, lastErr
}

// readCommands accepts control lines from the browser on stdin.
func readCommands(t *Tunnel) {
	sc := bufio.NewScanner(os.Stdin)
	for sc.Scan() {
		switch strings.TrimSpace(sc.Text()) {
		case "RESCAN":
			go t.Optimize()
		}
	}
}

func watchParent(pid int) {
	for {
		if !processAlive(pid) {
			os.Exit(0)
		}
		time.Sleep(time.Second)
	}
}
