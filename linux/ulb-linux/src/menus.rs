use gtk4::gio::{self, Menu, MenuItem};
use ulb_core::config::AppSettings;
use ulb_core::warp_host::WarpStatus;

/// Snapshot of window & feature state needed to construct the `⋯` main menu.
pub struct MainMenuContext<'a> {
    pub settings: &'a AppSettings,
    pub warp: &'a WarpStatus,
    pub sidebar_visible: bool,
    pub fullscreen: bool,
    pub reader_active: bool,
    pub web_page: bool,
    pub tab_zoom: f64,
    pub host_adblock_on: bool,
    pub tab_count: usize,
    pub extension_count: usize,
    pub script_count: usize,
    pub headphones_detected: bool,
}

/// Builds the native GTK4 `⋯` main application menu matching `src/MainWindowMenus.cpp`.
pub fn build_main_menu(ctx: &MainMenuContext<'_>) -> Menu {
    let settings = ctx.settings;
    let warp = ctx.warp;
    let root = Menu::new();

    // Section 1: Memorial offline snapshot
    let s_mem = Menu::new();
    s_mem.append(Some("六四事件（离线版）"), Some("win.memorial"));
    root.append_section(None, &s_mem);

    // Section 2: Windows & tabs
    let s_win = Menu::new();
    s_win.append(Some("新建标签页\tCtrl+T"), Some("win.new-tab"));
    s_win.append(Some("新建窗口\tCtrl+N"), Some("win.new-window"));
    s_win.append(Some("新建无痕浏览窗口\tCtrl+Shift+N"), Some("win.new-private"));
    root.append_section(None, &s_win);

    // Section 3: Library
    let s_lib = Menu::new();
    s_lib.append(Some("编辑书签\tCtrl+Shift+B"), Some("win.bookmarks"));
    s_lib.append(Some("显示所有历史记录\tCtrl+H"), Some("win.history"));
    s_lib.append(Some("显示阅读列表"), Some("win.reading-list"));
    s_lib.append(
        Some(if ctx.sidebar_visible { "隐藏边栏\tCtrl+Shift+L" } else { "显示边栏\tCtrl+Shift+L" }),
        Some("win.toggle-sidebar"),
    );
    s_lib.append(Some("显示标签页概览\tCtrl+Shift+\\"), Some("win.overview"));
    root.append_section(None, &s_lib);

    // Section 4: Page & Zoom & Features
    let s_page = Menu::new();
    let reader_item = MenuItem::new(
        Some(if ctx.reader_active { "隐藏阅读器\tCtrl+Shift+R" } else { "显示阅读器\tCtrl+Shift+R" }),
        if ctx.web_page { Some("win.toggle-reader") } else { None },
    );
    s_page.append_item(&reader_item);
    s_page.append(Some("查找…\tCtrl+F"), Some("win.find"));

    let zoom_sub = Menu::new();
    zoom_sub.append(Some("实际大小\tCtrl+0"), Some("win.zoom-reset"));
    zoom_sub.append(Some("放大\tCtrl++"), Some("win.zoom-in"));
    zoom_sub.append(Some("缩小\tCtrl+-"), Some("win.zoom-out"));
    s_page.append_submenu(Some(&format!("缩放 ({:.0}%)", ctx.tab_zoom * 100.0)), &zoom_sub);

    let blocker_sub = Menu::new();
    blocker_sub.append(
        Some(if settings.enable_ad_block { "✓ 全局内容拦截器已开启" } else { "开启全局内容拦截器" }),
        Some("win.toggle-global-adblock"),
    );
    if ctx.web_page {
        blocker_sub.append(
            Some(if ctx.host_adblock_on { "✓ 在此网站启用拦截器" } else { "在此网站启用拦截器" }),
            Some("win.toggle-site-adblock"),
        );
    }
    s_page.append_submenu(Some("内容拦截器"), &blocker_sub);

    let audio_sub = Menu::new();
    let dev_status = if ctx.headphones_detected { "当前检测：耳机" } else { "当前检测：音箱/线路" };
    audio_sub.append_item(&MenuItem::new(Some(dev_status), None));
    audio_sub.append(
        Some(if settings.system_audio_passthrough { "● 系统原声直通" } else { "○ 系统原声直通" }),
        Some("win.audio-passthrough"),
    );
    audio_sub.append(
        Some(if !settings.system_audio_passthrough && settings.enable_surround_sound {
            "● 空间音频 / Web Audio 增强"
        } else {
            "○ 空间音频 / Web Audio 增强"
        }),
        Some("win.audio-surround"),
    );
    audio_sub.append(
        Some(if settings.enable_vocal_boost { "✓ 人声增强" } else { "人声增强" }),
        Some("win.audio-vocal"),
    );
    audio_sub.append(
        Some(if settings.enable_night_mode { "✓ 夜间模式（防突响）" } else { "夜间模式（防突响）" }),
        Some("win.audio-night"),
    );
    s_page.append_submenu(
        Some(if settings.system_audio_passthrough { "声音 · 原声" } else { "声音 · 增强" }),
        &audio_sub,
    );

    let warp_sub = Menu::new();
    let status_line = if !settings.warp_enabled {
        "状态：直接连接".to_string()
    } else if warp.up {
        format!("状态：已连接 ({})", warp.endpoint)
    } else if !warp.error.is_empty() {
        format!("状态：{}", warp.error)
    } else {
        "状态：连接中…".to_string()
    };
    warp_sub.append_item(&MenuItem::new(Some(&status_line), None));
    if settings.warp_enabled && !settings.use_line {
        warp_sub.append(Some("重新优选 IP"), Some("win.warp-rescan"));
    }
    warp_sub.append(
        Some(if !settings.warp_enabled { "● 直接连接" } else { "○ 直接连接" }),
        Some("win.net-direct"),
    );
    warp_sub.append(
        Some(if settings.warp_enabled && !settings.use_line { "● Cloudflare WARP" } else { "○ Cloudflare WARP" }),
        Some("win.net-warp"),
    );
    warp_sub.append(
        Some(if settings.warp_enabled && settings.use_line { "● 我的线路" } else { "○ 我的线路" }),
        Some("win.net-line"),
    );
    warp_sub.append(Some("管理我的线路…"), Some("win.manage-lines"));
    warp_sub.append(
        Some(if settings.warp_fail_closed { "✓ WARP 断开时阻止联网" } else { "WARP 断开时阻止联网" }),
        Some("win.warp-fail-closed"),
    );
    let net_label = if !settings.warp_enabled {
        "网络线路 · 直接连接"
    } else if settings.use_line {
        "网络线路 · 我的线路"
    } else if warp.up {
        "网络线路 · WARP 已连接"
    } else {
        "网络线路 · WARP"
    };
    s_page.append_submenu(Some(net_label), &warp_sub);

    let ua_sub = Menu::new();
    let is_mac = settings.user_agent_profile == "macos-edge";
    ua_sub.append(
        Some(if !is_mac { "● 默认" } else { "○ 默认" }),
        Some("win.ua-default"),
    );
    ua_sub.append(
        Some(if is_mac { "● Microsoft Edge — macOS" } else { "○ Microsoft Edge — macOS" }),
        Some("win.ua-macos-edge"),
    );
    ua_sub.append(Some("检查当前浏览器UA标识…"), Some("win.ua-selftest"));
    s_page.append_submenu(
        Some(if is_mac { "浏览器UA标识 · macOS" } else { "浏览器UA标识 · 默认" }),
        &ua_sub,
    );

    let power_sub = Menu::new();
    for (mins, label, act) in [
        (0, "从不", "win.suspend-0"),
        (5, "5 分钟后", "win.suspend-5"),
        (10, "10 分钟后", "win.suspend-10"),
        (30, "30 分钟后", "win.suspend-30"),
        (60, "1 小时后", "win.suspend-60"),
    ] {
        let prefix = if settings.tab_suspend_minutes == mins { "● " } else { "○ " };
        power_sub.append(Some(&format!("{prefix}{label}")), Some(act));
    }
    if ctx.tab_count > 1 {
        power_sub.append(Some("立即挂起其他标签页"), Some("win.suspend-others"));
    }
    s_page.append_submenu(
        Some(if settings.tab_suspend_minutes > 0 { "节能 · 自动挂起" } else { "节能 · 关闭" }),
        &power_sub,
    );

    root.append_section(None, &s_page);

    // Section 5: Preferences & Info
    let s_app = Menu::new();
    s_app.append(
        Some(if ctx.fullscreen { "退出全屏幕\tF11" } else { "进入全屏幕\tF11" }),
        Some("win.toggle-fullscreen"),
    );
    s_app.append(
        Some(&format!("扩展和脚本 ({}/{})…", ctx.extension_count, ctx.script_count)),
        Some("win.extensions"),
    );
    s_app.append(Some("设置…\tCtrl+,"), Some("win.settings"));
    s_app.append(Some("关于 UltraLightBrowser"), Some("win.about"));
    root.append_section(None, &s_app);

    let _ = gio::Application::default();
    root
}

pub fn build_share_menu() -> Menu {
    let m = Menu::new();
    m.append(Some("拷贝链接"), Some("win.copy-url"));
    m.append(Some("添加书签…\tCtrl+D"), Some("win.add-bookmark"));
    m.append(Some("添加到个人收藏"), Some("win.add-favorite"));
    m.append(Some("添加到阅读列表\tCtrl+Shift+D"), Some("win.add-reading"));
    m
}

pub fn build_tab_menu(muted: bool, can_close_others: bool, has_closed: bool) -> Menu {
    let m = Menu::new();
    m.append(Some("重新载入"), Some("win.tab-ctx-reload"));
    m.append(Some("复制标签页"), Some("win.tab-ctx-duplicate"));
    m.append(
        Some(if muted { "取消标签页静音" } else { "将标签页静音" }),
        Some("win.tab-ctx-mute"),
    );
    m.append(Some("关闭标签页"), Some("win.tab-ctx-close"));
    if can_close_others {
        m.append(Some("关闭其他标签页"), Some("win.tab-ctx-close-others"));
    }
    if has_closed {
        m.append(Some("重新打开上次关闭的标签页\tCtrl+Shift+T"), Some("win.reopen-tab"));
    }
    m
}
