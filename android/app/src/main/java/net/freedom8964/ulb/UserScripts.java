package net.freedom8964.ulb;

import android.content.Context;
import android.content.SharedPreferences;
import android.text.TextUtils;
import android.webkit.WebView;

import androidx.webkit.ScriptHandler;
import androidx.webkit.WebViewCompat;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Persistent Tampermonkey-style user scripts and WebView document-start injection. */
final class UserScripts {
    private static final int MAX_SCRIPTS = 200;
    private static final int MAX_SCRIPT_BYTES = 2 * 1024 * 1024;
    private static final int MAX_TOTAL_BYTES = 4 * 1024 * 1024;
    static final class Entry {
        final String id, name, source;
        final boolean enabled;
        Entry(String id, String name, String source, boolean enabled) {
            this.id = id; this.name = name; this.source = source; this.enabled = enabled;
        }
    }

    private static final Pattern META = Pattern.compile("(?s)// ==UserScript==\\s*(.*?)// ==/UserScript==");
    private static final Pattern FIELD = Pattern.compile("(?m)^\\s*//\\s*@(\\S+)\\s+(.+?)\\s*$");
    private final SharedPreferences prefs;
    private final List<Entry> entries = new ArrayList<>();
    private final Map<WebView, List<ScriptHandler>> installed = new HashMap<>();

    UserScripts(Context context) {
        prefs = context.getSharedPreferences("user_scripts", Context.MODE_PRIVATE);
        load();
    }

    List<Entry> list() { return new ArrayList<>(entries); }

    String install(String source, String fallbackName, String defaultMatch) throws IOException {
        if (source.trim().isEmpty()) throw new IOException("脚本内容为空");
        int newBytes = source.getBytes(StandardCharsets.UTF_8).length;
        if (newBytes > MAX_SCRIPT_BYTES) throw new IOException("脚本超过 2 MB");
        if (entries.size() >= MAX_SCRIPTS) throw new IOException("最多保存 200 个脚本");
        int totalBytes = newBytes;
        for (Entry entry : entries) totalBytes += entry.source.getBytes(StandardCharsets.UTF_8).length;
        if (totalBytes > MAX_TOTAL_BYTES) throw new IOException("所有脚本合计不能超过 4 MB");
        String name = fallbackName;
        Matcher block = META.matcher(source);
        if (block.find()) {
            Matcher fields = FIELD.matcher(block.group(1));
            List<String> rules = new ArrayList<>();
            while (fields.find()) {
                String key = fields.group(1).toLowerCase(Locale.ROOT);
                String value = fields.group(2).trim();
                if (key.equals("name")) name = value;
                if (key.equals("match") || key.equals("include")) rules.add(value);
            }
            if (rules.isEmpty()) throw new IOException("脚本头部缺少 @match 或 @include，无法限定运行网站");
        } else if (TextUtils.isEmpty(defaultMatch)) {
            throw new IOException("普通 JS 脚本需要指定运行网站");
        } else {
            source = "// ==UserScript==\n// @name " + name + "\n// @match " + defaultMatch + "\n// ==/UserScript==\n" + source;
        }
        if (TextUtils.isEmpty(name)) name = "用户脚本";
        String id = UUID.randomUUID().toString();
        entries.add(new Entry(id, name, source, true));
        save();
        refreshAll();
        return name;
    }

    void setEnabled(String id, boolean enabled) {
        for (int i = 0; i < entries.size(); i++) {
            Entry e = entries.get(i);
            if (e.id.equals(id)) { entries.set(i, new Entry(e.id, e.name, e.source, enabled)); save(); refreshAll(); return; }
        }
    }

    void remove(String id) {
        entries.removeIf(e -> e.id.equals(id));
        save(); refreshAll();
    }

    void attach(WebView webView) {
        detach(webView);
        List<ScriptHandler> handlers = new ArrayList<>();
        for (Entry e : entries) if (e.enabled) {
            try { handlers.add(WebViewCompat.addDocumentStartJavaScript(webView, injection(e), java.util.Collections.singleton("*"))); }
            catch (RuntimeException ignored) { }
        }
        installed.put(webView, handlers);
    }

    void detach(WebView webView) {
        List<ScriptHandler> handlers = installed.remove(webView);
        if (handlers != null) for (ScriptHandler h : handlers) h.remove();
    }

    private void refreshAll() {
        for (WebView w : new ArrayList<>(installed.keySet())) attach(w);
        // Refresh currently displayed pages so changes take effect immediately.
        for (WebView w : new ArrayList<>(installed.keySet())) if (w.getUrl() != null) w.reload();
    }

    private String injection(Entry e) {
        JSONObject cfg = new JSONObject();
        try {
            cfg.put("id", e.id);
            cfg.put("name", e.name);
            cfg.put("source", e.source);
            cfg.put("matches", new JSONArray(rules(e.source, "match", "include")));
            cfg.put("excludes", new JSONArray(rules(e.source, "exclude", "exclude-match")));
            cfg.put("runAt", value(e.source, "run-at", "document-idle"));
            cfg.put("noframes", hasTag(e.source, "noframes"));
        } catch (Exception ignored) { }
        return "(function(c){try{" +
            "var u=location.href;"+
            "if(c.noframes&&window.top!==window.self)return;"+
            "function glob(p){if(p.charAt(0)==='/')return new RegExp(p.slice(1,p.lastIndexOf('/'))).test(u);"+
            "var x=p.replace(/[.+^${}()|[\\]\\\\]/g,'\\\\$&').replace(/\\*/g,'.*').replace(/\\?/g,'\\\\?');"+
            "return new RegExp('^'+x+'$','i').test(u)}"+
            "var ok=c.matches.some(glob)&&!c.excludes.some(glob);if(!ok)return;"+
            "function run(){try{var vals=JSON.parse(localStorage.getItem('__ulb_gm_'+c.id)||'{}');"+
            "var api={GM_info:{script:{name:c.name}},GM_getValue:function(k,d){return k in vals?vals[k]:d},"+
            "GM_setValue:function(k,v){vals[k]=v;localStorage.setItem('__ulb_gm_'+c.id,JSON.stringify(vals))},"+
            "GM_deleteValue:function(k){delete vals[k];localStorage.setItem('__ulb_gm_'+c.id,JSON.stringify(vals))},"+
            "GM_addStyle:function(s){var e=document.createElement('style');e.textContent=s;(document.head||document.documentElement).appendChild(e);return e}};"+
            "new Function('GM_info','GM_getValue','GM_setValue','GM_deleteValue','GM_addStyle',c.source)(api.GM_info,api.GM_getValue,api.GM_setValue,api.GM_deleteValue,api.GM_addStyle);"+
            "}catch(e){console.error('[UltraLight user script '+c.name+']',e)}}"+
            "if(c.runAt==='document-start')run();else if(c.runAt==='document-end'){if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',run,{once:true});else run()}else{if(document.readyState==='complete')setTimeout(run,0);else window.addEventListener('load',run,{once:true})}"+
            "}catch(e){console.error('[UltraLight user script]',e)}})(" + cfg.toString() + ");";
    }

    private List<String> rules(String source, String... wanted) {
        List<String> out = new ArrayList<>(); Matcher b = META.matcher(source); if (!b.find()) return out;
        Matcher m = FIELD.matcher(b.group(1));
        while (m.find()) for (String key : wanted) if (m.group(1).equalsIgnoreCase(key)) {
            String rule = m.group(2).trim();
            out.add(rule);
            // Chrome-style @match host wildcards include the bare domain too.
            if (key.equals("match") && rule.contains("://*.")) out.add(rule.replace("://*.", "://"));
        }
        if (out.isEmpty()) {
            String d = prefs.getString("default_match", "");
            if (!d.isEmpty()) out.add(d);
        }
        return out;
    }

    private String value(String source, String key, String fallback) {
        Matcher b = META.matcher(source); if (!b.find()) return fallback;
        Matcher m = FIELD.matcher(b.group(1)); while (m.find()) if (m.group(1).equalsIgnoreCase(key)) return m.group(2).trim();
        return fallback;
    }

    private boolean hasTag(String source, String tag) {
        Matcher b = META.matcher(source); return b.find() && Pattern.compile("(?m)^\\s*//\\s*@" + Pattern.quote(tag) + "\\s*$", Pattern.CASE_INSENSITIVE).matcher(b.group(1)).find();
    }

    private void load() {
        JSONArray a;
        try { a = new JSONArray(prefs.getString("entries", "[]")); } catch (Exception e) { return; }
        for (int i = 0; i < a.length(); i++) try {
            JSONObject o = a.getJSONObject(i);
            entries.add(new Entry(o.getString("id"), o.getString("name"), o.getString("source"), o.optBoolean("enabled", true)));
        } catch (Exception ignored) { }
    }

    private void save() {
        JSONArray a = new JSONArray();
        try { for (Entry e : entries) a.put(new JSONObject().put("id", e.id).put("name", e.name).put("source", e.source).put("enabled", e.enabled)); }
        catch (Exception ignored) { }
        prefs.edit().putString("entries", a.toString()).apply();
    }

    static String readFile(Context context, android.net.Uri uri) throws IOException {
        try (java.io.InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("无法读取文件");
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int total = 0, n;
            while ((n = in.read(buffer)) != -1) {
                total += n;
                if (total > MAX_SCRIPT_BYTES) throw new IOException("脚本超过 2 MB");
                out.write(buffer, 0, n);
            }
            byte[] bytes = out.toByteArray();
            return new String(bytes, StandardCharsets.UTF_8).replace("\uFEFF", "");
        }
    }
}
