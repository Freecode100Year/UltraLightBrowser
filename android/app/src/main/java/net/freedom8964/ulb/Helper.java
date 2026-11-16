package net.freedom8964.ulb;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.os.Handler;
import android.os.Looper;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * The network helper (warp/ in the repository, Go): Cloudflare WARP or a line,
 * served as a SOCKS5 proxy on 127.0.0.1 for the WebView. It ships as
 * lib/<abi>/libulbwarp.so so Android unpacks it where it may run.
 *
 * Status lines it prints: PORT n, STATE up|down, LINE host:port, ENDPOINT ..,
 * REASON noipv6|unreachable|handshake, ERROR text.
 */
final class Helper {
    interface Listener {
        void onPort(int port);
        void onStatus(boolean up, String detail);
    }

    private final Context ctx;
    private final Handler main = new Handler(Looper.getMainLooper());
    private Listener listener;
    private Process process;
    private volatile boolean stopped = true;
    private int port;
    private String mode;              // "warp" or "line"
    private List<String> links;       // line mode: one link, or several (automatic)
    private String rotatePrefix;      // public line only
    private int generation;           // bumped by stop(): stale readers stay quiet

    Helper(Context ctx) {
        this.ctx = ctx.getApplicationContext();
    }

    static File exe(Context ctx) {
        return new File(ctx.getApplicationInfo().nativeLibraryDir, "libulbwarp.so");
    }

    /** Starts (or restarts with new settings) on the same proxy port when one is known. */
    synchronized void start(String mode, List<String> links, String rotatePrefix, Listener listener) {
        stopLocked();
        this.mode = mode;
        this.links = links;
        this.rotatePrefix = rotatePrefix;
        this.listener = listener;
        stopped = false;
        launch(++generation);
    }

    synchronized void stop() {
        stopLocked();
        port = 0;
    }

    private void stopLocked() {
        stopped = true;
        generation++;
        if (process != null) {
            process.destroy();
            process = null;
        }
    }

    private void launch(int gen) {
        List<String> cmd = new ArrayList<>();
        cmd.add(exe(ctx).getAbsolutePath());
        cmd.add("-listen");
        cmd.add("127.0.0.1:" + port);  // 0 the first time; then keep the port the WebView uses
        cmd.add("-parent");
        cmd.add(String.valueOf(android.os.Process.myPid()));
        cmd.add("-fail-closed");       // never fall back to a direct connection
        if ("warp".equals(mode)) {
            cmd.add("-state");
            cmd.add(new File(ctx.getFilesDir(), "warp-account.json").getAbsolutePath());
        }
        ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true).directory(ctx.getFilesDir());
        Map<String, String> env = pb.environment();
        env.put("ULB_DNS", dnsServers(ctx));
        if ("line".equals(mode)) {
            if (links.size() > 1) {
                env.put("ULB_LINES", String.join("\n", links));
            } else {
                env.put("ULB_LINE", links.get(0));
                if (rotatePrefix != null) env.put("ULB_LINE_ROTATE", rotatePrefix);
            }
        }
        final Process p;
        try {
            p = pb.start();
        } catch (Exception e) {
            post(gen, () -> listener.onStatus(false, "网络组件无法启动：" + e.getMessage()));
            return;
        }
        synchronized (this) {
            if (gen != generation) {
                p.destroy();
                return;
            }
            process = p;
        }
        final long started = System.currentTimeMillis();
        new Thread(() -> {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                String reason = "";
                while ((line = r.readLine()) != null) {
                    if (line.startsWith("PORT ")) {
                        int n = Integer.parseInt(line.substring(5).trim());
                        synchronized (this) { if (gen == generation) port = n; }
                        post(gen, () -> listener.onPort(n));
                    } else if (line.equals("STATE up")) {
                        reason = "";
                        post(gen, () -> listener.onStatus(true, "已连接"));
                    } else if (line.equals("STATE down")) {
                        final String why = reason;
                        post(gen, () -> listener.onStatus(false, why.isEmpty() ? "未连接（已阻止联网）" : why));
                    } else if (line.startsWith("REASON ")) {
                        String w = line.substring(7);
                        reason = w.equals("noipv6") ? "未连接：本机没有 IPv6 网络，这条线路需要 IPv6"
                               : w.equals("unreachable") ? "未连接：连不到入口服务器"
                               : w.equals("handshake") ? "未连接：服务器拒绝了连接，链接可能已失效" : "";
                        final String why = reason;
                        if (!why.isEmpty()) post(gen, () -> listener.onStatus(false, why));
                    } else if (line.startsWith("ERROR line:")) {
                        final String err = "线路错误：" + line.substring(11).trim();
                        post(gen, () -> listener.onStatus(false, err));
                    }
                }
            } catch (Exception ignored) {
            }
            try { p.waitFor(); } catch (InterruptedException ignored) { }
            // It died: restart on the same port after a pause (longer if it keeps dying at once).
            long ran = System.currentTimeMillis() - started;
            synchronized (Helper.this) {
                if (stopped || gen != generation) return;
            }
            post(gen, () -> listener.onStatus(false, "网络组件已退出，正在重启…"));
            main.postDelayed(() -> {
                synchronized (Helper.this) {
                    if (stopped || gen != generation) return;
                }
                launch(gen);
            }, ran < 10000 ? 5000 : 1000);
        }, "helper-reader").start();
    }

    private void post(int gen, Runnable r) {
        main.post(() -> {
            synchronized (this) { if (gen != generation) return; }
            if (listener != null) r.run();
        });
    }

    /** The phone's DNS servers, for the helper's lookups outside the tunnel (warp/dns.go). */
    static String dnsServers(Context ctx) {
        List<String> out = new ArrayList<>();
        try {
            ConnectivityManager cm = ctx.getSystemService(ConnectivityManager.class);
            Network n = cm.getActiveNetwork();
            LinkProperties lp = n == null ? null : cm.getLinkProperties(n);
            if (lp != null) for (InetAddress a : lp.getDnsServers()) out.add(a.getHostAddress());
        } catch (Exception ignored) {
        }
        // Public resolvers as a last resort (the first answers in mainland China too).
        out.add("223.5.5.5");
        out.add("1.1.1.1");
        return String.join(",", out);
    }

    /** Runs a one-shot mode (-probe, -fetch) and returns its output. Blocking: call off the main thread. */
    static String runTool(Context ctx, List<String> args, Map<String, String> vars, long timeoutMs) {
        List<String> cmd = new ArrayList<>();
        cmd.add(exe(ctx).getAbsolutePath());
        cmd.addAll(args);
        ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true).directory(ctx.getFilesDir());
        pb.environment().put("ULB_DNS", dnsServers(ctx));
        pb.environment().putAll(vars);
        StringBuilder out = new StringBuilder();
        try {
            Process p = pb.start();
            Thread reader = new Thread(() -> {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = r.readLine()) != null) {
                        synchronized (out) { out.append(line).append('\n'); }
                    }
                } catch (Exception ignored) {
                }
            });
            reader.start();
            if (!p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
                p.destroyForcibly();
                synchronized (out) { out.append("ERROR 超时\n"); }
            }
            reader.join(2000);
        } catch (Exception e) {
            synchronized (out) { out.append("ERROR ").append(e.getMessage()).append('\n'); }
        }
        synchronized (out) { return out.toString(); }
    }
}
