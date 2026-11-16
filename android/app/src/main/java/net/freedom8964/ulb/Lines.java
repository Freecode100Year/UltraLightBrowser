package net.freedom8964.ulb;

import android.content.Context;
import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * "我的线路": the user's share links and subscriptions and which one is used,
 * like src/LineStore.cpp on Windows. Kept in the app's private storage
 * (lines.json), which other apps cannot read.
 */
final class Lines {
    static final String PUBLIC = "public";
    static final String AUTO = "auto";
    static final int AUTO_MAX = 20;
    static final int MAX_LINES = 500;

    static final class Line {
        String id, name, link, sub;
    }

    static final class Sub {
        String id, name, url;
        long updated;
        int count;
    }

    final List<Line> lines = new ArrayList<>();
    final List<Sub> subs = new ArrayList<>();
    String selected = PUBLIC;
    private long nextId = 1;
    private final File file;

    // The public line server answers on a new IPv6 address in this /64 every 3 days
    // (warp/rotate.go, server/ulb-line-ipv6.sh).
    static final String PUBLIC_ROTATE = "2001:470:1f07:5f5::/64";

    /** The built-in public line. Assembled from parts so scrapers that collect share links pass it over. */
    static String publicLine() {
        return "vl" + "ess://" + "5680cee3-6b91-4739-86b8-5aded88cd227" + "@[" + "2001:470:1f07:5f5:3bcd:d73f:3e79:6b3e" + "]:443" +
               "?encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.nvidia.com&fp=chrome" +
               "&pbk=" + "89rnPvfXmhlVxTRQot_US6EV7GWZLaNgTK2aDKINiW4" + "&sid=" + "14702eaec705a52e" + "&type=tcp";
    }

    Lines(Context ctx) {
        file = new File(ctx.getFilesDir(), "lines.json");
        load();
    }

    private void load() {
        try {
            JSONObject root = new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
            JSONArray ls = root.optJSONArray("lines");
            for (int i = 0; ls != null && i < ls.length(); i++) {
                JSONObject o = ls.getJSONObject(i);
                Line l = new Line();
                l.id = o.optString("id");
                l.name = o.optString("name");
                l.link = o.optString("link");
                l.sub = o.optString("sub");
                if (!l.id.isEmpty() && !l.link.isEmpty()) lines.add(l);
            }
            JSONArray ss = root.optJSONArray("subs");
            for (int i = 0; ss != null && i < ss.length(); i++) {
                JSONObject o = ss.getJSONObject(i);
                Sub s = new Sub();
                s.id = o.optString("id");
                s.name = o.optString("name");
                s.url = o.optString("url");
                s.updated = o.optLong("updated");
                s.count = o.optInt("count");
                if (!s.id.isEmpty() && !s.url.isEmpty()) subs.add(s);
            }
            nextId = Math.max(1, root.optLong("next", 1));
            if (!select(root.optString("selected", PUBLIC))) selected = PUBLIC;
        } catch (Exception ignored) {
        }
    }

    boolean save() {
        try {
            JSONArray ls = new JSONArray();
            for (Line l : lines) ls.put(new JSONObject().put("id", l.id).put("name", l.name).put("link", l.link).put("sub", l.sub));
            JSONArray ss = new JSONArray();
            for (Sub s : subs) ss.put(new JSONObject().put("id", s.id).put("name", s.name).put("url", s.url).put("updated", s.updated).put("count", s.count));
            JSONObject root = new JSONObject().put("lines", ls).put("subs", ss).put("selected", selected).put("next", nextId);
            File tmp = new File(file.getPath() + ".tmp");
            Files.write(tmp.toPath(), root.toString().getBytes(StandardCharsets.UTF_8));
            return tmp.renameTo(file);
        } catch (Exception e) {
            return false;
        }
    }

    Line find(String id) {
        for (Line l : lines) if (l.id.equals(id)) return l;
        return null;
    }

    /** Adds the links not in the list yet; returns how many were added. */
    int add(List<String> links, String sub) {
        int added = 0;
        for (String raw : links) {
            String link = raw.trim();
            if (link.isEmpty() || !link.contains("://") || lines.size() >= MAX_LINES) continue;
            boolean dup = false;
            for (Line l : lines) if (l.link.equals(link)) { dup = true; break; }
            if (dup) continue;
            Line l = new Line();
            l.id = "l" + (nextId++);
            l.name = nameOf(link);
            l.link = link;
            l.sub = sub == null ? "" : sub;
            lines.add(l);
            added++;
        }
        return added;
    }

    int addWireGuardProfile(String name, String canonicalLink) {
        int added = add(java.util.Collections.singletonList(canonicalLink), "");
        if (added > 0) {
            Line line = lines.get(lines.size() - 1);
            String label = name == null ? "" : name.trim();
            line.name = label.isEmpty() ? "WireGuard 配置" : label.replaceFirst("(?i)\\.conf$", "");
        }
        return added;
    }

    static String wireGuardLink(String config) {
        String encoded = Base64.encodeToString(config.getBytes(StandardCharsets.UTF_8),
            Base64.URL_SAFE | Base64.NO_PADDING | Base64.NO_WRAP);
        return "wireguard://" + encoded;
    }

    boolean remove(String id) {
        Line l = find(id);
        if (l == null) return false;
        lines.remove(l);
        if (selected.equals(id) || (selected.equals(AUTO) && lines.isEmpty())) selected = PUBLIC;
        return true;
    }

    boolean select(String id) {
        if (PUBLIC.equals(id) || (AUTO.equals(id) && !lines.isEmpty()) || find(id) != null) {
            selected = id;
            return true;
        }
        return false;
    }

    String addSub(String name, String url) {
        for (Sub s : subs) if (s.url.equals(url)) return s.id;
        Sub s = new Sub();
        s.id = "s" + (nextId++);
        s.name = name == null || name.trim().isEmpty() ? "订阅 " + (subs.size() + 1) : name.trim();
        s.url = url;
        subs.add(s);
        return s.id;
    }

    Sub findSub(String id) {
        for (Sub s : subs) if (s.id.equals(id)) return s;
        return null;
    }

    void removeSub(String id) {
        Sub s = findSub(id);
        if (s == null) return;
        subs.remove(s);
        for (Line l : new ArrayList<>(lines)) if (l.sub.equals(id)) remove(l.id);
    }

    /** Replaces a subscription's lines; a selected line that is still there stays selected. */
    void replaceSub(String id, List<String> links, long now) {
        Sub s = findSub(id);
        if (s == null) return;
        Line sel = find(selected);
        String keep = sel != null && sel.sub.equals(id) ? sel.link : null;
        lines.removeIf(l -> l.sub.equals(id));
        add(links, id);
        s.updated = now;
        s.count = 0;
        for (Line l : lines) if (l.sub.equals(id)) s.count++;
        if (keep != null) {
            selected = PUBLIC;
            for (Line l : lines) if (l.link.equals(keep)) selected = l.id;
        } else if (!select(selected)) {
            selected = PUBLIC;
        }
    }

    /** The links to run: empty means the public line; several means automatic selection. */
    List<String> selectedLinks() {
        List<String> out = new ArrayList<>();
        if (AUTO.equals(selected)) {
            for (Line l : lines) {
                if (out.size() >= AUTO_MAX) break;
                out.add(l.link);
            }
        } else {
            Line l = find(selected);
            if (l != null) out.add(l.link);
        }
        return out;
    }

    String selectedName() {
        if (PUBLIC.equals(selected)) return "内置公共线路";
        if (AUTO.equals(selected)) return "自动选择";
        Line l = find(selected);
        return l == null ? "内置公共线路" : l.name;
    }

    // ---- display helpers

    static String protocolOf(String link) {
        String s = scheme(link);
        switch (s) {
            case "vless": return "VLESS";
            case "trojan": return "Trojan";
            case "vmess": return "VMess";
            case "ss": return "Shadowsocks";
            case "hysteria2": case "hy2": return "Hysteria2";
            case "tuic": return "TUIC";
            case "wireguard": return "WireGuard";
            default: return s;
        }
    }

    static String nameOf(String link) {
        try {
            if (scheme(link).equals("vmess")) {
                String ps = vmess(link).optString("ps").trim();
                if (!ps.isEmpty()) return clip(ps);
            } else {
                int hash = link.indexOf('#');
                if (hash >= 0) {
                    String n = URLDecoder.decode(link.substring(hash + 1).replace("+", "%2B"), "UTF-8").replaceAll("[\\r\\n\\t]", " ").trim();
                    if (!n.isEmpty()) return clip(n);
                }
            }
        } catch (Exception ignored) {
        }
        String server = serverOf(link);
        return server.isEmpty() ? protocolOf(link) : protocolOf(link) + " " + server;
    }

    static String serverOf(String link) {
        try {
            String s = scheme(link);
            if (s.equals("wireguard")) return "wg-quick 配置";
            if (s.equals("vmess")) {
                JSONObject v = vmess(link);
                return v.optString("add").isEmpty() ? "" : v.optString("add") + ":" + v.optString("port");
            }
            String rest = link.substring(link.indexOf("://") + 3);
            int hash = rest.indexOf('#');
            if (hash >= 0) rest = rest.substring(0, hash);
            if (s.equals("ss") && !rest.contains("@")) {
                int q = rest.indexOf('?');
                rest = new String(b64(q >= 0 ? rest.substring(0, q) : rest), StandardCharsets.UTF_8);
            }
            int at = rest.lastIndexOf('@');
            if (at < 0) return "";
            String hp = rest.substring(at + 1);
            int cut = hp.length();
            for (char c : new char[]{'/', '?'}) {
                int i = hp.indexOf(c);
                if (i >= 0 && i < cut) cut = i;
            }
            return hp.substring(0, cut);
        } catch (Exception e) {
            return "";
        }
    }

    private static String scheme(String link) {
        int i = link.indexOf("://");
        return i < 0 ? "" : link.substring(0, i).toLowerCase(Locale.ROOT);
    }

    private static JSONObject vmess(String link) throws Exception {
        String b = link.substring(link.indexOf("://") + 3).trim();
        return new JSONObject(new String(b64(b), StandardCharsets.UTF_8));
    }

    /** Standard or URL-safe base64, padding optional. */
    static byte[] b64(String s) {
        return Base64.decode(s.trim().replace('-', '+').replace('_', '/'), Base64.DEFAULT);
    }

    private static String clip(String s) {
        return s.length() > 60 ? s.substring(0, 60) : s;
    }
}
