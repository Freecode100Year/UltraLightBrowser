package net.freedom8964.ulb;

import android.app.AlertDialog;
import android.net.Uri;
import android.content.res.Configuration;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.text.DateFormat;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.nio.charset.StandardCharsets;

/** "我的线路": choose, import, test and subscribe, like the desktop settings page. */
final class LinesDialog {
    private final MainActivity act;
    private Lines lines;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<String, String> probe = new HashMap<>();  // line id -> "123 ms" / "不可用" / "测速中…"
    private AlertDialog dialog;
    private LinearLayout list;
    private final boolean dark;

    LinesDialog(MainActivity act, Lines lines) {
        this.act = act;
        this.lines = lines;
        dark = (act.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, act.getResources().getDisplayMetrics()));
    }

    void show() {
        LinearLayout col = new LinearLayout(act);
        col.setOrientation(LinearLayout.VERTICAL);
        LinearLayout buttons = new LinearLayout(act);
        buttons.setPadding(dp(12), dp(4), dp(12), dp(4));
        buttons.addView(button("导入链接", v -> importLinks()), new LinearLayout.LayoutParams(0, -2, 1));
        buttons.addView(button("导入 WireGuard", v -> act.pickWireGuardFile(this)), new LinearLayout.LayoutParams(0, -2, 1));
        buttons.addView(button("添加订阅", v -> addSub()), new LinearLayout.LayoutParams(0, -2, 1));
        buttons.addView(button("全部测速", v -> runProbe(allIds())), new LinearLayout.LayoutParams(0, -2, 1));
        col.addView(buttons);
        ScrollView scroll = new ScrollView(act);
        list = new LinearLayout(act);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        col.addView(scroll, new LinearLayout.LayoutParams(-1, dp(380)));
        TextView hint = new TextView(act);
        hint.setText("支持 VLESS、Trojan、VMess、Shadowsocks、Hysteria2（hysteria2:// / hy2://）；不支持 TUIC。" +
            "传输支持 TCP / WebSocket / HTTPUpgrade / XHTTP / gRPC，安全 TLS / REALITY；证书校验始终开启。" +
            "WireGuard 仅导入单 Peer wg-quick .conf，必须填写 DNS 且 DNS 地址需在 AllowedIPs 内；网页 DNS 经隧道。Hysteria2 / WireGuard 的服务器域名由设备 DNS 解析。" +
            "订阅支持通用分享链接，不支持 WireGuard 文件或 Clash YAML。线路保存在本应用私有存储中。");
        hint.setTextSize(12);
        hint.setAlpha(0.7f);
        hint.setPadding(dp(20), dp(6), dp(20), dp(4));
        col.addView(hint);
        dialog = new AlertDialog.Builder(act).setTitle("我的线路").setView(col).setPositiveButton("完成", null).create();
        dialog.show();
        render();
    }

    void importWireGuardFile(Uri uri) {
        if (uri == null) return;
        new Thread(() -> {
            try {
                String name = "WireGuard 配置";
                android.database.Cursor cursor = act.getContentResolver().query(uri, null, null, null, null);
                if (cursor != null) {
                    try {
                        int column = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                        if (column >= 0 && cursor.moveToFirst()) name = cursor.getString(column);
                    } finally { cursor.close(); }
                }
                String config;
                try (InputStream in = act.getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new IllegalArgumentException("无法读取所选文件");
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096];
                    int total = 0, count;
                    while ((count = in.read(buffer)) != -1) {
                        total += count;
                        if (total > 64 * 1024) throw new IllegalArgumentException("WireGuard 配置不能超过 64 KB");
                        out.write(buffer, 0, count);
                    }
                    config = new String(out.toByteArray(), StandardCharsets.UTF_8);
                }
                String canonical = Lines.wireGuardLink(config);
                String result = Helper.runTool(act, Arrays.asList("-probe", "-parse-only"),
                    Collections.singletonMap("ULB_PROBE", canonical), 15000);
                String status = parseProbe(result).get(0);
                String finalName = name;
                main.post(() -> {
                    if (!"ok".equals(status)) {
                        act.toast("WireGuard 配置无效：" + (status == null ? "无法检查" : status.replaceFirst("^fail ", "")));
                        return;
                    }
                    int added = lines.addWireGuardProfile(finalName, canonical);
                    lines.save();
                    render();
                    act.toast(added == 0 ? "此 WireGuard 配置已存在" : "WireGuard 配置已导入");
                });
            } catch (Exception e) {
                main.post(() -> act.toast("导入 WireGuard 配置失败：" + e.getMessage()));
            }
        }).start();
    }

    private TextView button(String text, View.OnClickListener click) {
        TextView b = new TextView(act);
        b.setText(text);
        b.setTextColor(0xFF0A84FF);
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
        b.setPadding(0, dp(10), 0, dp(10));
        b.setOnClickListener(click);
        return b;
    }

    private List<String> allIds() {
        List<String> ids = new ArrayList<>();
        for (Lines.Line l : lines.lines) ids.add(l.id);
        return ids;
    }

    private void render() {
        list.removeAllViews();
        list.addView(row(Lines.PUBLIC, "内置公共线路", "需要 IPv6 网络 · 入口每 3 天更换", null));
        if (!lines.lines.isEmpty()) {
            list.addView(row(Lines.AUTO, "自动选择", "启动时测试前 " + Lines.AUTO_MAX + " 条，用最快的；断开时自动换", null));
        }
        for (Lines.Sub s : lines.subs) {
            TextView t = new TextView(act);
            t.setText("订阅：" + s.name + "（" + s.count + " 条" + (s.updated > 0 ? "，" + DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(new Date(s.updated)) : "") + "）  更新 · 删除");
            t.setTextSize(13);
            t.setTextColor(0xFF0A84FF);
            t.setPadding(dp(20), dp(10), dp(20), dp(4));
            t.setOnClickListener(v -> new AlertDialog.Builder(act).setTitle(s.name)
                .setItems(new String[]{"更新订阅", "删除订阅和它的线路"}, (d, which) -> {
                    if (which == 0) fetch(s.url, s.name, s.id);
                    else { lines.removeSub(s.id); saveAndApply(false); }
                }).show());
            list.addView(t);
        }
        for (Lines.Line l : lines.lines) {
            String sub = "";
            if (!l.sub.isEmpty()) {
                Lines.Sub s = lines.findSub(l.sub);
                if (s != null) sub = " · " + s.name;
            }
            list.addView(row(l.id, l.name, Lines.protocolOf(l.link) + " · " + Lines.serverOf(l.link) + sub, l));
        }
        if (lines.lines.isEmpty()) {
            TextView empty = new TextView(act);
            empty.setText("还没有自己的线路。点“导入链接”粘贴分享链接，或“添加订阅”。");
            empty.setAlpha(0.6f);
            empty.setPadding(dp(20), dp(16), dp(20), dp(16));
            list.addView(empty);
        }
    }

    private View row(String id, String name, String detail, Lines.Line line) {
        LinearLayout r = new LinearLayout(act);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(20), dp(10), dp(16), dp(10));
        boolean sel = lines.selected.equals(id);
        if (sel) {
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(dark ? 0x330A84FF : 0x1A0A84FF);
            r.setBackground(bg);
        }
        LinearLayout text = new LinearLayout(act);
        text.setOrientation(LinearLayout.VERTICAL);
        TextView n = new TextView(act);
        n.setText((sel ? "✓ " : "") + name);
        n.setTextSize(16);
        n.setSingleLine(true);
        n.setEllipsize(TextUtils.TruncateAt.END);
        TextView d = new TextView(act);
        d.setText(detail);
        d.setTextSize(12);
        d.setAlpha(0.6f);
        d.setSingleLine(true);
        d.setEllipsize(TextUtils.TruncateAt.END);
        text.addView(n);
        text.addView(d);
        r.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
        String p = probe.get(id);
        if (p != null) {
            TextView lat = new TextView(act);
            lat.setText(p);
            lat.setTextSize(12);
            lat.setTextColor(p.endsWith("ms") ? (Integer.parseInt(p.replace(" ms", "")) < 400 ? 0xFF34C759 : 0xFFFF9F0A) : p.equals("不可用") ? 0xFFFF453A : 0xFF8E8E93);
            r.addView(lat);
        }
        r.setOnClickListener(v -> {
            lines.select(id);
            saveAndApply(true);
            act.toast("已改用“" + name + "”");
        });
        if (line != null) {
            r.setOnLongClickListener(v -> {
                new AlertDialog.Builder(act).setTitle(name).setItems(new String[]{"测速", "重命名", "删除"}, (dlg, which) -> {
                    if (which == 0) runProbe(Collections.singletonList(id));
                    else if (which == 1) rename(line);
                    else { lines.remove(id); saveAndApply(false); }
                }).show();
                return true;
            });
        }
        return r;
    }

    /** Saves; switches the browser to "我的线路" when a line was chosen, else restarts it if in use. */
    private void saveAndApply(boolean chosen) {
        lines.save();
        if (chosen) act.setNetMode("line");
        else act.linesChanged();
        lines = new Lines(act);
        render();
    }

    private void rename(Lines.Line l) {
        EditText e = new EditText(act);
        e.setText(l.name);
        e.setSingleLine(true);
        new AlertDialog.Builder(act).setTitle("重命名").setView(e)
            .setPositiveButton("好", (d, w) -> {
                String n = e.getText().toString().trim();
                l.name = n.isEmpty() ? Lines.nameOf(l.link) : n;
                lines.save();
                render();
            }).setNegativeButton("取消", null).show();
    }

    private void importLinks() {
        EditText e = new EditText(act);
        e.setHint("每行一条分享链接");
        e.setMinLines(4);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        new AlertDialog.Builder(act).setTitle("导入线路链接").setView(e)
            .setPositiveButton("导入", (d, w) -> {
                List<String> links = new ArrayList<>();
                for (String s : e.getText().toString().split("\\r?\\n")) if (!s.trim().isEmpty()) links.add(s.trim());
                if (links.isEmpty()) return;
                if (links.size() > 64) { act.toast("一次最多导入 64 条，订阅请用“添加订阅”"); return; }
                act.toast("正在检查链接…");
                new Thread(() -> {
                    String out = Helper.runTool(act, Arrays.asList("-probe", "-parse-only"), Collections.singletonMap("ULB_PROBE", String.join("\n", links)), 15000);
                    Map<Integer, String> res = parseProbe(out);
                    List<String> good = new ArrayList<>();
                    StringBuilder bad = new StringBuilder();
                    for (int i = 0; i < links.size(); i++) {
                        String r = res.get(i);
                        if ("ok".equals(r)) good.add(links.get(i));
                        else bad.append("• ").append(Lines.nameOf(links.get(i))).append("：")
                                .append(r == null ? "无法检查" : r.startsWith("fail ") ? r.substring(5) : r).append('\n');
                    }
                    main.post(() -> {
                        int added = lines.add(good, "");
                        lines.save();
                        render();
                        String msg = "已导入 " + added + " 条" + (good.size() > added ? "，" + (good.size() - added) + " 条已存在" : "");
                        if (bad.length() > 0) {
                            new AlertDialog.Builder(act).setTitle(msg).setMessage("以下链接无法使用：\n\n" + bad).setPositiveButton("好", null).show();
                        } else {
                            act.toast(msg);
                        }
                    });
                }).start();
            }).setNegativeButton("取消", null).show();
    }

    private void addSub() {
        LinearLayout col = new LinearLayout(act);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), 0, dp(20), 0);
        EditText url = new EditText(act);
        url.setHint("订阅地址 https://…");
        url.setSingleLine(true);
        url.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        EditText name = new EditText(act);
        name.setHint("名称（可不填）");
        name.setSingleLine(true);
        col.addView(url);
        col.addView(name);
        new AlertDialog.Builder(act).setTitle("添加订阅").setView(col)
            .setPositiveButton("添加", (d, w) -> {
                String u = url.getText().toString().trim();
                if (!u.startsWith("https://") && !u.startsWith("http://")) { act.toast("订阅地址要以 https:// 开头"); return; }
                fetch(u, name.getText().toString(), null);
            }).setNegativeButton("取消", null).show();
    }

    /** Downloads a subscription with the helper (through the running proxy first, then directly). */
    private void fetch(String url, String name, String subId) {
        act.toast("正在下载订阅…");
        new Thread(() -> {
            List<String> args = new ArrayList<>(Collections.singletonList("-fetch"));
            int port = act.proxyPort();
            if (port > 0) { args.add("-socks"); args.add("127.0.0.1:" + port); }
            String out = Helper.runTool(act, args, Collections.singletonMap("ULB_FETCH", url), 70000);
            List<String> links = new ArrayList<>();
            int skipped = -1;
            String error = "";
            for (String line : out.split("\n")) {
                if (line.startsWith("LINK ")) links.add(line.substring(5));
                else if (line.startsWith("FETCHED ")) skipped = Integer.parseInt(line.substring(line.lastIndexOf(' ') + 1).trim());
                else if (line.startsWith("ERROR ")) error = line.substring(6);
            }
            final int sk = skipped;
            final String err = error.replaceFirst("^fetch: ", "");
            main.post(() -> {
                if (sk < 0) {
                    new AlertDialog.Builder(act).setTitle("订阅下载失败").setMessage(err.isEmpty() ? "未知错误" : err).setPositiveButton("好", null).show();
                    return;
                }
                String id = subId != null ? subId : lines.addSub(name, url);
                lines.replaceSub(id, links, System.currentTimeMillis());
                saveAndApply(false);
                act.toast("订阅有 " + links.size() + " 条线路" + (sk > 0 ? "，" + sk + " 条格式不支持已跳过" : ""));
            });
        }).start();
    }

    private void runProbe(List<String> ids) {
        List<String> targets = new ArrayList<>();
        List<String> links = new ArrayList<>();
        for (String id : ids) {
            Lines.Line l = lines.find(id);
            if (l == null || targets.size() >= 64) continue;
            targets.add(id);
            links.add(l.link);
            probe.put(id, "测速中…");
        }
        if (targets.isEmpty()) return;
        render();
        new Thread(() -> {
            String out = Helper.runTool(act, Collections.singletonList("-probe"), Collections.singletonMap("ULB_PROBE", String.join("\n", links)), 90000);
            Map<Integer, String> res = parseProbe(out);
            main.post(() -> {
                for (int i = 0; i < targets.size(); i++) {
                    String r = res.get(i);
                    probe.put(targets.get(i), r == null || r.startsWith("fail") ? "不可用" : r.trim() + " ms");
                }
                if (dialog.isShowing()) render();
            });
        }).start();
    }

    static Map<Integer, String> parseProbe(String out) {
        Map<Integer, String> m = new HashMap<>();
        for (String line : out.split("\n")) {
            if (!line.startsWith("PROBE ")) continue;
            String[] parts = line.split(" ", 3);
            if (parts.length < 3) continue;
            try { m.put(Integer.parseInt(parts[1]), parts[2]); } catch (NumberFormatException ignored) { }
        }
        return m;
    }
}
