package net.freedom8964.ulb;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Message;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.PermissionRequest;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.webkit.ProxyConfig;
import androidx.webkit.ProxyController;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * UltraLightBrowser for Android: the system WebView with every request sent
 * through the bundled network helper (WARP or a line), a Safari-style bottom
 * bar and tab overview. Nothing is kept after the browser closes.
 */
public class MainActivity extends Activity implements Helper.Listener {
    static final String HOME = "https://search.brave.com/";
    // WebRTC could reach the network outside the proxy and reveal the real address.
    private static final String NO_WEBRTC =
        "(function(){try{['RTCPeerConnection','webkitRTCPeerConnection','RTCDataChannel','RTCIceCandidate','RTCSessionDescription']" +
        ".forEach(function(k){try{Object.defineProperty(window,k,{value:undefined,writable:false,configurable:false});}catch(e){}});}catch(e){}})();";

    static final class Tab {
        WebView view;
        String title = "";
        Bitmap thumb;
        boolean failed;  // the last page load failed (e.g. while the line was still connecting)
    }

    private final List<Tab> tabs = new ArrayList<>();
    private Tab current;
    private FrameLayout root, content, overview;
    private LinearLayout bar;
    private TextView address, statusDot, tabCount;
    private EditText input;
    private ImageButton back, forward, reload;
    private ProgressBar progress;

    private Helper helper;
    private Lines lines;
    private LinesDialog activeLinesDialog;
    private UserScripts userScripts;
    private SharedPreferences prefs;
    private String netMode;                 // "warp", "line" or "direct"
    private boolean proxyReady;             // the WebView's proxy points at the running helper
    private int port;                       // the helper's proxy port, 0 when none
    private boolean netUp;
    private String netDetail = "正在连接…";
    private final List<Object[]> pending = new ArrayList<>();  // {Tab, url} to open once the line is up
    private ValueCallback<Uri[]> fileCallback;
    private boolean dark;

    // ------------------------------------------------------------------ lifecycle

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        userScripts = new UserScripts(this);
        netMode = prefs.getString("net", "warp");
        lines = new Lines(this);
        helper = new Helper(this);
        dark = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        buildUi();
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebView.setWebContentsDebuggingEnabled(true);
        }
        String start = urlFromIntent(getIntent());
        newTab(start != null ? start : HOME, true);
        startNetwork();
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::goBack);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String url = urlFromIntent(intent);
        if (url != null) newTab(url, true);
    }

    @Override
    protected void onPause() {
        super.onPause();
        for (Tab t : tabs) t.view.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (current != null) current.view.onResume();
    }

    @Override
    protected void onDestroy() {
        if (isFinishing()) clearEverything();
        helper.stop();
        for (Tab t : tabs) { userScripts.detach(t.view); t.view.destroy(); }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        goBack();
    }

    private void goBack() {
        if (overview.getVisibility() == View.VISIBLE) {
            hideOverview();
        } else if (input.getVisibility() == View.VISIBLE) {
            endEditing();
        } else if (current != null && current.view.canGoBack()) {
            current.view.goBack();
        } else if (tabs.size() > 1) {
            closeTab(current);
        } else {
            finish();
        }
    }

    /** Like closing the desktop browser: cookies, site data, cache and history go. */
    private void clearEverything() {
        CookieManager.getInstance().removeAllCookies(null);
        CookieManager.getInstance().flush();
        WebStorage.getInstance().deleteAllData();
        for (Tab t : tabs) {
            t.view.clearCache(true);
            t.view.clearHistory();
            t.view.clearFormData();
        }
    }

    // ------------------------------------------------------------------ network

    private void startNetwork() {
        lines = new Lines(this);  // the lines dialog may have changed them
        proxyReady = false;
        netUp = false;
        netDetail = "正在连接…";
        updateStatus();
        if (netMode.equals("direct")) {
            helper.stop();
            port = 0;
            if (WebViewFeature.isFeatureSupported(WebViewFeature.PROXY_OVERRIDE)) {
                ProxyController.getInstance().clearProxyOverride(Runnable::run, this::proxySet);
            } else {
                proxySet();
            }
            netUp = true;
            netDetail = "直接连接（网站能看到你的真实 IP）";
            updateStatus();
            return;
        }
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.PROXY_OVERRIDE)) {
            // Without a proxy every page would go out directly, revealing the real address.
            netDetail = "手机的 Android System WebView 版本太旧，无法安全翻墙。请在应用商店更新 “Android System WebView” 或 Chrome。";
            updateStatus();
            showBlockedPage(netDetail);
            return;
        }
        List<String> links;
        String rotate = null;
        if (netMode.equals("line")) {
            links = lines.selectedLinks();
            if (links.isEmpty()) {
                links = Collections.singletonList(Lines.publicLine());
                rotate = Lines.PUBLIC_ROTATE;
            }
        } else {
            links = Collections.emptyList();
        }
        // Until the helper listens, pages would go nowhere: nothing loads before onPort.
        helper.start(netMode, links, rotate, this);
    }

    @Override
    public void onPort(int port) {
        this.port = port;
        ProxyConfig cfg = new ProxyConfig.Builder().addProxyRule("socks5://127.0.0.1:" + port).build();
        ProxyController.getInstance().setProxyOverride(cfg, Runnable::run, this::proxySet);
    }

    private void proxySet() {
        runOnUiThread(() -> {
            proxyReady = true;
            // Wait for the line to come up before the first page; a page loaded now
            // would only fail. After 30 s load anyway so the error shows.
            if (netUp) flushPending();
            else root.postDelayed(this::flushPending, 30000);
        });
    }

    private void flushPending() {
        if (!proxyReady) return;
        for (Object[] p : pending) {
            Tab t = (Tab) p[0];
            if (tabs.contains(t)) t.view.loadUrl((String) p[1]);
        }
        pending.clear();
    }

    @Override
    public void onStatus(boolean up, String detail) {
        boolean wasUp = netUp;
        netUp = up;
        netDetail = detail;
        updateStatus();
        if (up) flushPending();
        // Pages that failed while the line was down load again now.
        if (up && !wasUp) {
            for (Tab t : tabs) {
                if (t.failed) {
                    t.failed = false;
                    t.view.reload();
                }
            }
        }
    }

    private void updateStatus() {
        int color = netMode.equals("direct") ? 0xFFFF9F0A : netUp ? 0xFF34C759 : proxyReady ? 0xFFFF453A : 0xFF8E8E93;
        statusDot.setTextColor(color);
    }

    private String modeName() {
        return netMode.equals("warp") ? "Cloudflare WARP" : netMode.equals("line") ? "我的线路 · " + lines.selectedName() : "直接连接";
    }

    int proxyPort() {
        return port;
    }

    void setNetMode(String mode) {
        netMode = mode;
        prefs.edit().putString("net", mode).apply();
        startNetwork();
        toast("已切换到 " + modeName());
    }

    /** Called by LinesActivity-like dialog after the selection changed. */
    void linesChanged() {
        if (netMode.equals("line")) startNetwork();
        else lines = new Lines(this);
    }

    private void showBlockedPage(String why) {
        String html = "<meta name=viewport content='width=device-width'><body style='font:16px sans-serif;padding:24px;line-height:1.6'>" +
            "<h3>为保护你的真实 IP，已停止联网</h3><p>" + TextUtils.htmlEncode(why) + "</p></body>";
        if (current != null) current.view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null);
    }

    // ------------------------------------------------------------------ tabs

    private Tab newTab(String url, boolean activate) {
        Tab t = new Tab();
        t.view = createWebView(t);
        tabs.add(t);
        content.addView(t.view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (activate) activate(t);
        else t.view.setVisibility(View.GONE);
        if (url != null) load(t, url);
        updateTabCount();
        return t;
    }

    private void load(Tab t, String url) {
        if (proxyReady && netUp) {
            t.view.loadUrl(url);
        } else {
            pending.add(new Object[]{t, url});
        }
    }

    private void activate(Tab t) {
        if (current != null && current != t) {
            current.thumb = snapshot(current.view);  // for the tab overview
            current.view.setVisibility(View.GONE);
            current.view.onPause();
        }
        current = t;
        t.view.setVisibility(View.VISIBLE);
        t.view.onResume();
        refreshBar();
    }

    private void closeTab(Tab t) {
        int i = tabs.indexOf(t);
        if (i < 0) return;
        tabs.remove(i);
        content.removeView(t.view);
        userScripts.detach(t.view);
        t.view.destroy();
        if (tabs.isEmpty()) {
            current = null;
            newTab(HOME, true);
        } else if (t == current) {
            current = null;
            activate(tabs.get(Math.min(i, tabs.size() - 1)));
        }
        updateTabCount();
    }

    private WebView createWebView(Tab tab) {
        WebView w = new WebView(this);
        WebSettings s = w.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setSupportMultipleWindows(true);
        s.setJavaScriptCanOpenWindowsAutomatically(false);
        s.setLoadWithOverviewMode(true);
        s.setUseWideViewPort(true);
        s.setBuiltInZoomControls(true);
        s.setDisplayZoomControls(false);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setGeolocationEnabled(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        s.setSafeBrowsingEnabled(false);
        // Look like Chrome rather than an embedded WebView ("; wv", "Version/4.0").
        s.setUserAgentString(s.getUserAgentString().replace("; wv", "").replaceAll(" Version/\\d+\\.\\d+", ""));
        CookieManager.getInstance().setAcceptThirdPartyCookies(w, false);
        if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            WebViewCompat.addDocumentStartJavaScript(w, NO_WEBRTC, Collections.singleton("*"));
            userScripts.attach(w);
        }
        w.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
                Uri u = req.getUrl();
                String scheme = u.getScheme() == null ? "" : u.getScheme();
                if (scheme.equals("http") || scheme.equals("https") || scheme.equals("about") || scheme.equals("data")) return false;
                // tel:, mailto:, intent:, market: ... go to the app that handles them.
                try {
                    Intent i = scheme.equals("intent") ? Intent.parseUri(u.toString(), Intent.URI_INTENT_SCHEME) : new Intent(Intent.ACTION_VIEW, u);
                    i.addCategory(Intent.CATEGORY_BROWSABLE);
                    i.setComponent(null);
                    i.setSelector(null);
                    startActivity(i);
                } catch (Exception e) {
                    toast("没有可以打开此链接的应用");
                }
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest req, android.webkit.WebResourceError err) {
                if (req.isForMainFrame()) tab.failed = true;
            }

            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                tab.failed = false;
                if (!WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) view.evaluateJavascript(NO_WEBRTC, null);
                if (tab == current) refreshBar();
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (tab == current) refreshBar();
            }

            @Override
            public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) {
                if (tab == current) refreshBar();
            }
        });
        w.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onProgressChanged(WebView view, int p) {
                if (tab != current) return;
                progress.setProgress(p);
                progress.setVisibility(p >= 100 ? View.INVISIBLE : View.VISIBLE);
                reload.setImageResource(p >= 100 ? R.drawable.ic_reload : R.drawable.ic_stop);
            }

            @Override
            public void onReceivedTitle(WebView view, String title) {
                tab.title = title;
            }

            @Override
            public boolean onCreateWindow(WebView view, boolean dialog, boolean userGesture, Message resultMsg) {
                if (!userGesture) return false;  // pop-ups need a tap
                Tab t = newTab(null, true);
                ((WebView.WebViewTransport) resultMsg.obj).setWebView(t.view);
                resultMsg.sendToTarget();
                return true;
            }

            @Override
            public void onCloseWindow(WebView window) {
                for (Tab t : new ArrayList<>(tabs)) if (t.view == window) closeTab(t);
            }

            @Override
            public void onPermissionRequest(PermissionRequest request) {
                request.deny();  // camera and microphone: not supported yet
            }

            @Override
            public void onGeolocationPermissionsShowPrompt(String origin, GeolocationPermissions.Callback callback) {
                callback.invoke(origin, false, false);
            }

            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback, FileChooserParams params) {
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = callback;
                try {
                    startActivityForResult(params.createIntent(), 7);
                } catch (Exception e) {
                    fileCallback = null;
                    return false;
                }
                return true;
            }
        });
        w.setDownloadListener((url, ua, disposition, mime, length) -> {
            // The system downloader would fetch the file outside the proxy.
            copy(url);
            toast("暂不支持下载，已复制下载链接");
        });
        return w;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == REQUEST_WIREGUARD) {
            LinesDialog target = activeLinesDialog;
            activeLinesDialog = null;
            if (result == RESULT_OK && data != null && data.getData() != null && target != null) target.importWireGuardFile(data.getData());
            return;
        }
        if (request == REQUEST_SCRIPT) {
            if (result == RESULT_OK && data != null && data.getData() != null) importUserScript(data.getData());
            return;
        }
        if (request == 7 && fileCallback != null) {
            fileCallback.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(result, data));
            fileCallback = null;
            return;
        }
        super.onActivityResult(request, result, data);
    }

    // ------------------------------------------------------------------ UI

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private int barColor() { return dark ? 0xFF1C1C1E : 0xFFF2F2F7; }
    private int pillColor() { return dark ? 0xFF3A3A3C : 0xFFE3E3E8; }
    private int textColor() { return dark ? 0xFFFFFFFF : 0xFF000000; }
    private int accent() { return 0xFF0A84FF; }

    private ImageButton iconButton(int icon, View.OnClickListener click) {
        ImageButton b = new ImageButton(this);
        b.setImageResource(icon);
        b.setColorFilter(accent());
        TypedValue tv = new TypedValue();
        getTheme().resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, tv, true);
        b.setBackgroundResource(tv.resourceId);
        b.setOnClickListener(click);
        return b;
    }

    private void buildUi() {
        root = new FrameLayout(this);
        root.setBackgroundColor(barColor());
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        root.addView(column, new FrameLayout.LayoutParams(-1, -1));

        content = new FrameLayout(this);
        content.setBackgroundColor(dark ? Color.BLACK : Color.WHITE);
        column.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));

        bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setBackgroundColor(barColor());
        column.addView(bar, new LinearLayout.LayoutParams(-1, -2));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        progress.setVisibility(View.INVISIBLE);
        bar.addView(progress, new LinearLayout.LayoutParams(-1, dp(3)));

        // Address pill: status dot, site name (tap to edit), reload.
        FrameLayout pillBox = new FrameLayout(this);
        GradientDrawable pill = new GradientDrawable();
        pill.setColor(pillColor());
        pill.setCornerRadius(dp(12));
        pillBox.setBackground(pill);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(-1, dp(44));
        plp.setMargins(dp(12), dp(6), dp(12), dp(2));
        bar.addView(pillBox, plp);

        statusDot = new TextView(this);
        statusDot.setText("●");
        statusDot.setTextSize(14);
        statusDot.setGravity(Gravity.CENTER);
        statusDot.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle(modeName()).setMessage(netDetail)
            .setPositiveButton("网络线路…", (d, x) -> showNetMenu(statusDot)).setNegativeButton("关闭", null).show());
        pillBox.addView(statusDot, new FrameLayout.LayoutParams(dp(40), -1, Gravity.START));

        address = new TextView(this);
        address.setTextColor(textColor());
        address.setTextSize(16);
        address.setGravity(Gravity.CENTER);
        address.setSingleLine(true);
        address.setEllipsize(TextUtils.TruncateAt.END);
        address.setOnClickListener(v -> startEditing());
        FrameLayout.LayoutParams alp = new FrameLayout.LayoutParams(-1, -1);
        alp.setMargins(dp(40), 0, dp(40), 0);
        pillBox.addView(address, alp);

        input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(16);
        input.setTextColor(textColor());
        input.setHint("搜索或输入网址");
        input.setBackground(null);
        input.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING);
        input.setInputType(android.text.InputType.TYPE_TEXT_VARIATION_URI);
        input.setSelectAllOnFocus(true);
        input.setVisibility(View.GONE);
        input.setOnEditorActionListener((v, action, ev) -> {
            if (action == EditorInfo.IME_ACTION_GO || (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER)) {
                String url = resolve(input.getText().toString());
                endEditing();
                if (url != null && current != null) load(current, url);
                return true;
            }
            return false;
        });
        FrameLayout.LayoutParams ilp = new FrameLayout.LayoutParams(-1, -1);
        ilp.setMargins(dp(40), 0, dp(12), 0);
        pillBox.addView(input, ilp);

        reload = iconButton(R.drawable.ic_reload, v -> {
            if (current == null) return;
            if (current.view.getProgress() < 100) current.view.stopLoading();
            else current.view.reload();
        });
        pillBox.addView(reload, new FrameLayout.LayoutParams(dp(40), -1, Gravity.END));

        // Toolbar: back, forward, share, menu, tabs.
        LinearLayout tools = new LinearLayout(this);
        tools.setGravity(Gravity.CENTER_VERTICAL);
        bar.addView(tools, new LinearLayout.LayoutParams(-1, dp(48)));
        back = iconButton(R.drawable.ic_back, v -> { if (current != null && current.view.canGoBack()) current.view.goBack(); });
        forward = iconButton(R.drawable.ic_forward, v -> { if (current != null && current.view.canGoForward()) current.view.goForward(); });
        ImageButton share = iconButton(R.drawable.ic_share, v -> share());
        ImageButton menu = iconButton(R.drawable.ic_menu, this::showMenu);
        tabCount = new TextView(this);
        tabCount.setGravity(Gravity.CENTER);
        tabCount.setTextColor(accent());
        tabCount.setTextSize(13);
        tabCount.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        GradientDrawable box = new GradientDrawable();
        box.setStroke(dp(2), accent());
        box.setCornerRadius(dp(5));
        tabCount.setBackground(box);
        tabCount.setOnClickListener(v -> showOverview());
        FrameLayout tabsBox = new FrameLayout(this);
        tabsBox.addView(tabCount, new FrameLayout.LayoutParams(dp(24), dp(24), Gravity.CENTER));
        tabsBox.setOnClickListener(v -> showOverview());
        for (View v : new View[]{back, forward, share, menu, tabsBox}) tools.addView(v, new LinearLayout.LayoutParams(0, -1, 1));

        overview = new FrameLayout(this);
        overview.setBackgroundColor(dark ? 0xFF000000 : 0xFFE5E5EA);
        overview.setVisibility(View.GONE);
        root.addView(overview, new FrameLayout.LayoutParams(-1, -1));

        // Edge to edge: keep the page clear of the status bar, the bar clear of the
        // navigation bar and the keyboard.
        root.setOnApplyWindowInsetsListener((v, in) -> {
            Insets sys = in.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            Insets ime = in.getInsets(WindowInsets.Type.ime());
            column.setPadding(sys.left, sys.top, sys.right, 0);
            bar.setPadding(0, 0, 0, Math.max(sys.bottom, ime.bottom));
            overview.setPadding(sys.left, sys.top, sys.right, sys.bottom);
            return in;
        });
        setContentView(root);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= 30) {
            int light = android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
            getWindow().getInsetsController().setSystemBarsAppearance(dark ? 0 : light, light);
            getWindow().setDecorFitsSystemWindows(false);
        }
    }

    private void refreshBar() {
        if (current == null) return;
        String url = current.view.getUrl();
        String host = url == null ? "" : Uri.parse(url).getHost();
        address.setText(host == null || host.isEmpty() ? "搜索或输入网址" : host.replaceFirst("^www\\.", ""));
        back.setAlpha(current.view.canGoBack() ? 1f : 0.35f);
        forward.setAlpha(current.view.canGoForward() ? 1f : 0.35f);
        int p = current.view.getProgress();
        progress.setProgress(p);
        progress.setVisibility(p >= 100 ? View.INVISIBLE : View.VISIBLE);
    }

    private void updateTabCount() {
        tabCount.setText(String.valueOf(tabs.size()));
    }

    private void startEditing() {
        address.setVisibility(View.INVISIBLE);
        reload.setVisibility(View.GONE);
        input.setVisibility(View.VISIBLE);
        String url = current == null ? null : current.view.getUrl();
        input.setText(url == null ? "" : url);
        input.requestFocus();
        input.selectAll();
        getSystemService(InputMethodManager.class).showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
    }

    private void endEditing() {
        getSystemService(InputMethodManager.class).hideSoftInputFromWindow(input.getWindowToken(), 0);
        input.setVisibility(View.GONE);
        address.setVisibility(View.VISIBLE);
        reload.setVisibility(View.VISIBLE);
    }

    /** What was typed: an address, or a search. */
    static String resolve(String text) {
        String t = text.trim();
        if (t.isEmpty()) return null;
        if (t.matches("(?i)^(https?|about|data):.*")) return t;
        if (!t.contains(" ") && (t.matches("^[^/]+\\.[a-zA-Z]{2,}(/.*)?$") || t.matches("^\\[?[0-9a-fA-F:.]+\\]?(:\\d+)?(/.*)?$") || t.startsWith("localhost"))) {
            return "https://" + t;
        }
        try {
            return "https://www.google.com/search?q=" + URLEncoder.encode(t, StandardCharsets.UTF_8.name());
        } catch (Exception e) {
            return null;
        }
    }

    private String urlFromIntent(Intent intent) {
        if (intent == null) return null;
        if (Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) {
            String s = intent.getData().getScheme();
            if ("http".equals(s) || "https".equals(s)) return intent.getDataString();
        }
        if (Intent.ACTION_WEB_SEARCH.equals(intent.getAction())) {
            String q = intent.getStringExtra("query");
            if (q != null) return resolve(q);
        }
        return null;
    }

    // ------------------------------------------------------------------ overview

    private void showOverview() {
        if (current != null) current.thumb = snapshot(current.view);
        overview.removeAllViews();
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        overview.addView(col, new FrameLayout.LayoutParams(-1, -1));
        ScrollView scroll = new ScrollView(this);
        col.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(2);
        grid.setPadding(dp(8), dp(8), dp(8), dp(8));
        scroll.addView(grid);
        int cardW = (getResources().getDisplayMetrics().widthPixels - dp(16)) / 2 - dp(16);
        for (Tab t : tabs) {
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(dark ? 0xFF2C2C2E : Color.WHITE);
            bg.setCornerRadius(dp(14));
            if (t == current) bg.setStroke(dp(3), accent());
            card.setBackground(bg);
            card.setClipToOutline(true);
            card.setPadding(dp(6), dp(4), dp(6), dp(6));
            LinearLayout head = new LinearLayout(this);
            head.setGravity(Gravity.CENTER_VERTICAL);
            TextView title = new TextView(this);
            title.setText(TextUtils.isEmpty(t.title) ? "新标签页" : t.title);
            title.setSingleLine(true);
            title.setEllipsize(TextUtils.TruncateAt.END);
            title.setTextColor(textColor());
            title.setTextSize(13);
            head.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
            ImageButton close = iconButton(R.drawable.ic_close, v -> { closeTab(t); showOverview(); });
            close.setColorFilter(dark ? 0xFFAAAAAA : 0xFF666666);
            head.addView(close, new LinearLayout.LayoutParams(dp(32), dp(32)));
            card.addView(head);
            ImageView img = new ImageView(this);
            img.setScaleType(ImageView.ScaleType.CENTER_CROP);
            if (t.thumb != null) img.setImageBitmap(t.thumb);
            else img.setBackgroundColor(dark ? 0xFF3A3A3C : 0xFFF2F2F7);
            card.addView(img, new LinearLayout.LayoutParams(-1, (int) (cardW * 1.3f)));
            card.setOnClickListener(v -> { activate(t); hideOverview(); });
            GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
            glp.width = cardW;
            glp.setMargins(dp(8), dp(8), dp(8), dp(8));
            grid.addView(card, glp);
        }
        LinearLayout bottom = new LinearLayout(this);
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottom.setBackgroundColor(barColor());
        TextView count = new TextView(this);
        count.setText(tabs.size() + " 个标签页");
        count.setTextColor(textColor());
        count.setGravity(Gravity.CENTER);
        ImageButton add = iconButton(R.drawable.ic_plus, v -> { hideOverview(); newTab(HOME, true); });
        TextView done = new TextView(this);
        done.setText("完成");
        done.setTextColor(accent());
        done.setTextSize(17);
        done.setGravity(Gravity.CENTER);
        done.setOnClickListener(v -> hideOverview());
        bottom.addView(add, new LinearLayout.LayoutParams(0, dp(52), 1));
        bottom.addView(count, new LinearLayout.LayoutParams(0, dp(52), 1));
        bottom.addView(done, new LinearLayout.LayoutParams(0, dp(52), 1));
        col.addView(bottom, new LinearLayout.LayoutParams(-1, -2));
        overview.setVisibility(View.VISIBLE);
    }

    private void hideOverview() {
        overview.setVisibility(View.GONE);
        overview.removeAllViews();
    }

    private Bitmap snapshot(WebView w) {
        try {
            if (w.getWidth() == 0 || w.getHeight() == 0) return null;
            float scale = 0.35f;
            Bitmap b = Bitmap.createBitmap((int) (w.getWidth() * scale), (int) (w.getHeight() * scale), Bitmap.Config.RGB_565);
            Canvas c = new Canvas(b);
            c.scale(scale, scale);
            c.translate(-w.getScrollX(), -w.getScrollY());
            w.draw(c);
            return b;
        } catch (Throwable e) {
            return null;
        }
    }

    // ------------------------------------------------------------------ menus

    private static final int M_NEW = 1, M_COPY = 2, M_NET = 3, M_LINES = 4, M_ABOUT = 5, M_FIND = 6, M_CLEAR = 7, M_SCRIPTS = 8;

    private void showMenu(View anchor) {
        PopupMenu pm = new PopupMenu(this, anchor);
        Menu m = pm.getMenu();
        m.add(0, M_NEW, 0, "新建标签页");
        m.add(0, M_COPY, 1, "拷贝链接");
        m.add(0, M_FIND, 2, "在页面中查找");
        m.add(0, M_SCRIPTS, 3, "用户脚本…");
        m.add(0, M_NET, 4, "网络线路：" + modeName());
        m.add(0, M_LINES, 5, "管理我的线路…");
        m.add(0, M_CLEAR, 6, "清除浏览数据并退出");
        m.add(0, M_ABOUT, 7, "关于");
        pm.setOnMenuItemClickListener(this::onMenu);
        pm.show();
    }

    private boolean onMenu(MenuItem item) {
        switch (item.getItemId()) {
            case M_NEW: newTab(HOME, true); return true;
            case M_COPY: if (current != null && current.view.getUrl() != null) { copy(current.view.getUrl()); toast("已拷贝链接"); } return true;
            case M_FIND: find(); return true;
            case M_SCRIPTS: showUserScripts(); return true;
            case M_NET: showNetMenu(address); return true;
            case M_LINES: showLinesDialog(); return true;
            case M_CLEAR: finish(); return true;
            case M_ABOUT:
                new AlertDialog.Builder(this).setTitle("UltraLight 浏览器 " + versionName())
                    .setMessage("自带 Cloudflare WARP 和线路的翻墙浏览器。所有网页都经网络线路访问，线路不通时不会直接联网。关闭浏览器时清除 Cookie、网站数据和缓存。\n\n" +
                        "当前：" + modeName() + "\n状态：" + netDetail + "\n\ngitlab.com/sj9292008133/UltraLightBrowser")
                    .setPositiveButton("好", null).show();
                return true;
        }
        return false;
    }

    private static final int REQUEST_SCRIPT = 8;
    private static final int REQUEST_WIREGUARD = 9;

    void pickWireGuardFile(LinesDialog owner) {
        activeLinesDialog = owner;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"text/plain", "application/octet-stream"})
            .addCategory(Intent.CATEGORY_OPENABLE);
        try { startActivityForResult(pick, REQUEST_WIREGUARD); }
        catch (Exception e) { activeLinesDialog = null; toast("无法打开文件选择器"); }
    }

    private void showLinesDialog() {
        activeLinesDialog = new LinesDialog(this, lines);
        activeLinesDialog.show();
    }

    private void showUserScripts() {
        List<UserScripts.Entry> entries = userScripts.list();
        List<String> labels = new ArrayList<>();
        for (UserScripts.Entry e : entries) labels.add((e.enabled ? "✓  " : "○  ") + e.name);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("用户脚本（" + entries.size() + "）")
            .setItems(labels.toArray(new String[0]), (d, which) -> {
                UserScripts.Entry e = entries.get(which);
                userScripts.setEnabled(e.id, !e.enabled);
                showUserScripts();
            })
            .setPositiveButton("导入 JS 脚本…", (d, w) -> pickUserScript())
            .setNeutralButton("管理说明", null)
            .setNegativeButton("关闭", null).create();
        dialog.setOnShowListener(x -> dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            new AlertDialog.Builder(this).setTitle("用户脚本说明")
                .setMessage("选择 .user.js 或 .js 文件导入。油猴脚本需在头部声明 @match 或 @include；普通 JS 脚本导入时可指定运行网址。点脚本切换启用状态，长按脚本可删除。\n\n支持常用 GM_info、GM_getValue、GM_setValue、GM_deleteValue、GM_addStyle。脚本只在当前浏览器配置文件中保存。")
                .setPositiveButton("好", null).show();
        }));
        dialog.show();
        dialog.getListView().setOnItemLongClickListener((parent, view, position, id) -> {
            UserScripts.Entry e = entries.get(position);
            new AlertDialog.Builder(this).setTitle("删除脚本")
                .setMessage("删除“" + e.name + "”？")
                .setPositiveButton("删除", (x, y) -> { userScripts.remove(e.id); showUserScripts(); })
                .setNegativeButton("取消", null).show();
            return true;
        });
    }

    private void pickUserScript() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("text/*")
            .addCategory(Intent.CATEGORY_OPENABLE);
        try { startActivityForResult(i, REQUEST_SCRIPT); }
        catch (Exception e) { toast("无法打开文件选择器"); }
    }

    private void importUserScript(Uri uri) {
        String name = "用户脚本";
        try {
            android.database.Cursor c = getContentResolver().query(uri, null, null, null, null);
            if (c != null) {
                try { int n = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME); if (n >= 0 && c.moveToFirst()) name = c.getString(n); }
                finally { c.close(); }
            }
            final String scriptName = name;
            String source = UserScripts.readFile(this, uri);
            if (source.contains("==UserScript==")) {
                userScripts.install(source, scriptName, "");
                toast("脚本已导入：" + scriptName);
                showUserScripts();
            } else {
                EditText match = new EditText(this);
                match.setSingleLine(true);
                match.setHint("例如 https://example.com/*");
                final String scriptSource = source;
                new AlertDialog.Builder(this).setTitle("指定脚本运行网址")
                    .setMessage("普通 JS 没有油猴匹配规则，请限定它运行的页面。")
                    .setView(match).setPositiveButton("导入", (d, w) -> {
                        try { userScripts.install(scriptSource, scriptName, match.getText().toString().trim()); toast("脚本已导入：" + scriptName); showUserScripts(); }
                        catch (Exception e) { toast(e.getMessage()); }
                    }).setNegativeButton("取消", null).show();
            }
        } catch (Exception e) { toast("导入失败：" + e.getMessage()); }
    }

    private void showNetMenu(View anchor) {
        String[] names = {"Cloudflare WARP", "我的线路（" + lines.selectedName() + "）", "直接连接（不翻墙）"};
        String[] modes = {"warp", "line", "direct"};
        int checked = netMode.equals("line") ? 1 : netMode.equals("direct") ? 2 : 0;
        new AlertDialog.Builder(this).setTitle("网络线路")
            .setSingleChoiceItems(names, checked, (d, which) -> {
                d.dismiss();
                if (!modes[which].equals(netMode)) setNetMode(modes[which]);
            })
            .setNeutralButton("管理我的线路…", (d, w) -> showLinesDialog())
            .setNegativeButton("取消", null).show();
    }

    private void find() {
        if (current == null) return;
        EditText q = new EditText(this);
        q.setSingleLine(true);
        new AlertDialog.Builder(this).setTitle("在页面中查找").setView(q)
            .setPositiveButton("查找", (d, w) -> {
                current.view.findAllAsync(q.getText().toString());
                current.view.setFindListener((active, count, done) -> { if (done) toast(count == 0 ? "没有找到" : "共 " + count + " 处"); });
            })
            .setNegativeButton("取消", (d, w) -> current.view.clearMatches()).show();
    }

    private void share() {
        if (current == null || current.view.getUrl() == null) return;
        Intent i = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, current.view.getUrl());
        startActivity(Intent.createChooser(i, "分享"));
    }

    void copy(String text) {
        getSystemService(ClipboardManager.class).setPrimaryClip(ClipData.newPlainText("link", text));
    }

    void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }
}
