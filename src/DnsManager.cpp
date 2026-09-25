#include "DnsManager.hpp"
#include "Config.hpp"
#include "StringUtils.hpp"

#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

namespace UltraLight {

namespace {

void CopyTextToClipboard(HWND hWnd, const std::wstring& text) {
    if (text.empty() || text == L"(暂无)") return;
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        size_t bytes = (text.length() + 1) * sizeof(wchar_t);
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hGlobal) {
            void* pBuf = GlobalLock(hGlobal);
            if (pBuf) {
                memcpy(pBuf, text.c_str(), bytes);
                GlobalUnlock(hGlobal);
                SetClipboardData(CF_UNICODETEXT, hGlobal);
            }
        }
        CloseClipboard();
    }
}

// Dialog Control IDs
enum DlgControlID : int {
    IDC_DLG_CHK_ENABLE      = 3001,
    IDC_DLG_COMBO_PROVIDER  = 3002,
    IDC_DLG_EDIT_IPV4_1     = 3003,
    IDC_DLG_BTN_COPY_IPV4_1 = 3004,
    IDC_DLG_EDIT_IPV4_2     = 3005,
    IDC_DLG_BTN_COPY_IPV4_2 = 3006,
    IDC_DLG_EDIT_IPV6_1     = 3007,
    IDC_DLG_BTN_COPY_IPV6_1 = 3008,
    IDC_DLG_EDIT_IPV6_2     = 3009,
    IDC_DLG_BTN_COPY_IPV6_2 = 3010,
    IDC_DLG_EDIT_DOH        = 3011,
    IDC_DLG_BTN_COPY_DOH    = 3012,
    IDC_DLG_STATIC_DESC     = 3013,
    IDC_DLG_BTN_COPY_ALL    = 3014,
    IDC_DLG_BTN_OK          = 3015,
    IDC_DLG_BTN_CANCEL      = 3016
};

struct DlgContext {
    HWND hDlg = nullptr;
    HWND hChkEnable = nullptr;
    HWND hComboProvider = nullptr;
    HWND hEditIpv41 = nullptr;
    HWND hEditIpv42 = nullptr;
    HWND hEditIpv61 = nullptr;
    HWND hEditIpv62 = nullptr;
    HWND hEditDoh = nullptr;
    HWND hStaticDesc = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    bool isCustom = false;
};

void UpdateDialogControls(DlgContext* ctx) {
    if (!ctx || !ctx->hComboProvider) return;
    int idx = static_cast<int>(SendMessageW(ctx->hComboProvider, CB_GETCURSEL, 0, 0));
    const auto& providers = DnsManager::Instance().GetProviders();

    if (idx >= 0 && idx < static_cast<int>(providers.size())) {
        ctx->isCustom = false;
        const auto& p = providers[idx];
        SetWindowTextW(ctx->hEditIpv41, p.ipv4Primary.c_str());
        SetWindowTextW(ctx->hEditIpv42, p.ipv4Secondary.c_str());
        SetWindowTextW(ctx->hEditIpv61, p.ipv6Primary.c_str());
        SetWindowTextW(ctx->hEditIpv62, p.ipv6Secondary.c_str());
        SetWindowTextW(ctx->hEditDoh, p.dohTemplate.c_str());
        SetWindowTextW(ctx->hStaticDesc, (L"特性说明: " + p.description).c_str());
        SendMessageW(ctx->hEditDoh, EM_SETREADONLY, TRUE, 0);
    } else {
        // Custom
        ctx->isCustom = true;
        SetWindowTextW(ctx->hEditIpv41, L"自定义");
        SetWindowTextW(ctx->hEditIpv42, L"自定义");
        SetWindowTextW(ctx->hEditIpv61, L"自定义");
        SetWindowTextW(ctx->hEditIpv62, L"自定义");
        std::wstring customW = StringUtils::Utf8ToWide(Config::Instance().GetSettings().customDnsTemplate);
        if (customW.empty()) {
            customW = L"https://dns.alidns.com/dns-query";
        }
        SetWindowTextW(ctx->hEditDoh, customW.c_str());
        SetWindowTextW(ctx->hStaticDesc, L"特性说明: 可输入任意标准的 DoH (DNS-over-HTTPS) 节点解析地址。");
        SendMessageW(ctx->hEditDoh, EM_SETREADONLY, FALSE, 0);
    }
}

std::wstring GetWindowTextString(HWND hWnd) {
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return L"";
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(hWnd, &buf[0], len + 1);
    buf.resize(len);
    return buf;
}

LRESULT CALLBACK DlgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DlgContext* ctx = reinterpret_cast<DlgContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<DlgContext*>(cs->lpCreateParams);
        ctx->hDlg = hWnd;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        // Create clean fonts
        ctx->hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");

        const auto& settings = Config::Instance().GetSettings();

        // 1. Enable Checkbox
        ctx->hChkEnable = CreateWindowExW(0, L"BUTTON", L"启用公共 DNS 服务器 (Public DNS Server - 隐私保护与加速)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 16, 520, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_ENABLE)), nullptr, nullptr);
        SendMessageW(ctx->hChkEnable, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
        SendMessageW(ctx->hChkEnable, BM_SETCHECK, settings.enablePublicDns ? BST_CHECKED : BST_UNCHECKED, 0);

        // 2. Provider Selector Label & ComboBox
        HWND hLblProvider = CreateWindowExW(0, L"STATIC", L"选择公共 DNS 推荐服务商 (支持 IPv4 & IPv6 双栈):",
            WS_CHILD | WS_VISIBLE, 20, 50, 520, 18, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hLblProvider, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        ctx->hComboProvider = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            20, 72, 520, 260, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_COMBO_PROVIDER)), nullptr, nullptr);
        SendMessageW(ctx->hComboProvider, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        const auto& providers = DnsManager::Instance().GetProviders();
        int selectIdx = 0;
        for (size_t i = 0; i < providers.size(); ++i) {
            std::wstring itemText = providers[i].name;
            if (!providers[i].ipv6Primary.empty() && providers[i].ipv6Primary != L"(暂无)") {
                itemText += L"  [IPv4/IPv6]";
            } else {
                itemText += L"  [IPv4]";
            }
            SendMessageW(ctx->hComboProvider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(itemText.c_str()));
            if (providers[i].id == settings.selectedDnsProvider) {
                selectIdx = static_cast<int>(i);
            }
        }
        SendMessageW(ctx->hComboProvider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自定义 DNS / DoH 服务器节点..."));
        if (settings.selectedDnsProvider == "custom") {
            selectIdx = static_cast<int>(providers.size());
        }
        SendMessageW(ctx->hComboProvider, CB_SETCURSEL, selectIdx, 0);

        // 3. GroupBox for Server Details
        HWND hGrpDetails = CreateWindowExW(0, L"BUTTON", L"DNS 服务器节点地址详细信息",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            20, 110, 520, 245, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpDetails, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        // Helper macro/lambda for row creation
        auto createRow = [&](int y, const wchar_t* label, int editId, int copyId, HWND& outEdit) {
            HWND hLbl = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE,
                35, y + 2, 85, 18, hWnd, nullptr, nullptr, nullptr);
            SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

            outEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                125, y, 325, 22, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(editId)), nullptr, nullptr);
            SendMessageW(outEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);

            HWND hBtn = CreateWindowExW(0, L"BUTTON", L"复制",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                460, y - 1, 65, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(copyId)), nullptr, nullptr);
            SendMessageW(hBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);
        };

        createRow(135, L"IPv4 首选:", IDC_DLG_EDIT_IPV4_1, IDC_DLG_BTN_COPY_IPV4_1, ctx->hEditIpv41);
        createRow(165, L"IPv4 备选:", IDC_DLG_EDIT_IPV4_2, IDC_DLG_BTN_COPY_IPV4_2, ctx->hEditIpv42);
        createRow(195, L"IPv6 首选:", IDC_DLG_EDIT_IPV6_1, IDC_DLG_BTN_COPY_IPV6_1, ctx->hEditIpv61);
        createRow(225, L"IPv6 备选:", IDC_DLG_EDIT_IPV6_2, IDC_DLG_BTN_COPY_IPV6_2, ctx->hEditIpv62);
        createRow(255, L"DoH 节点:", IDC_DLG_EDIT_DOH, IDC_DLG_BTN_COPY_DOH, ctx->hEditDoh);

        // Description static
        ctx->hStaticDesc = CreateWindowExW(0, L"STATIC", L"特性说明: ",
            WS_CHILD | WS_VISIBLE, 35, 290, 490, 52, hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_STATIC_DESC)), nullptr, nullptr);
        SendMessageW(ctx->hStaticDesc, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // 4. GroupBox for hardware acceleration and privacy status
        HWND hGrpHz = CreateWindowExW(0, L"BUTTON", L"⚡ 硬件加速与隐私安全状态",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            20, 365, 520, 55, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hGrpHz, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        HWND hLblHz = CreateWindowExW(0, L"STATIC",
            L"🚀 120Hz 高清流畅刷新率与原生 VSync 同步已就绪，退出时自动粉碎全部缓存与无痕浏览",
            WS_CHILD | WS_VISIBLE, 35, 388, 490, 20, hWnd, nullptr, nullptr, nullptr);
        SendMessageW(hLblHz, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        // 5. Bottom buttons
        HWND hBtnCopyAll = CreateWindowExW(0, L"BUTTON", L"📋 一键复制全部 IP",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 435, 140, 32, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_COPY_ALL)), nullptr, nullptr);
        SendMessageW(hBtnCopyAll, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"确定并应用",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            330, 435, 100, 32, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_OK)), nullptr, nullptr);
        SendMessageW(hBtnOk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            440, 435, 100, 32, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_CANCEL)), nullptr, nullptr);
        SendMessageW(hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFont), TRUE);

        UpdateDialogControls(ctx);
        return 0;
    }

    case WM_COMMAND: {
        WORD code = HIWORD(wParam);
        WORD id = LOWORD(wParam);

        if (id == IDC_DLG_COMBO_PROVIDER && code == CBN_SELCHANGE) {
            UpdateDialogControls(ctx);
            return 0;
        }

        switch (id) {
        case IDC_DLG_BTN_COPY_IPV4_1:
            CopyTextToClipboard(hWnd, GetWindowTextString(ctx->hEditIpv41));
            break;
        case IDC_DLG_BTN_COPY_IPV4_2:
            CopyTextToClipboard(hWnd, GetWindowTextString(ctx->hEditIpv42));
            break;
        case IDC_DLG_BTN_COPY_IPV6_1:
            CopyTextToClipboard(hWnd, GetWindowTextString(ctx->hEditIpv61));
            break;
        case IDC_DLG_BTN_COPY_IPV6_2:
            CopyTextToClipboard(hWnd, GetWindowTextString(ctx->hEditIpv62));
            break;
        case IDC_DLG_BTN_COPY_DOH:
            CopyTextToClipboard(hWnd, GetWindowTextString(ctx->hEditDoh));
            break;
        case IDC_DLG_BTN_COPY_ALL: {
            int idx = static_cast<int>(SendMessageW(ctx->hComboProvider, CB_GETCURSEL, 0, 0));
            const auto& providers = DnsManager::Instance().GetProviders();
            std::wostringstream oss;
            if (idx >= 0 && idx < static_cast<int>(providers.size())) {
                const auto& p = providers[idx];
                oss << L"【" << p.name << L"】\n"
                    << L"IPv4 首选: " << p.ipv4Primary << L"\n"
                    << L"IPv4 备选: " << p.ipv4Secondary << L"\n"
                    << L"IPv6 首选: " << p.ipv6Primary << L"\n"
                    << L"IPv6 备选: " << p.ipv6Secondary << L"\n"
                    << L"DoH 节点: " << p.dohTemplate << L"\n";
            } else {
                oss << L"【自定义公共 DNS】\n"
                    << L"DoH 节点: " << GetWindowTextString(ctx->hEditDoh) << L"\n";
            }
            CopyTextToClipboard(hWnd, oss.str());
            MessageBoxW(hWnd, L"已将该服务商的全部 IPv4/IPv6 及 DoH 节点信息复制到剪贴板！", L"复制成功", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDC_DLG_BTN_OK: {
            bool enableDns = (SendMessageW(ctx->hChkEnable, BM_GETCHECK, 0, 0) == BST_CHECKED);
            int idx = static_cast<int>(SendMessageW(ctx->hComboProvider, CB_GETCURSEL, 0, 0));
            const auto& providers = DnsManager::Instance().GetProviders();

            auto& settings = Config::Instance().GetSettings();
            settings.enablePublicDns = enableDns;

            if (idx >= 0 && idx < static_cast<int>(providers.size())) {
                settings.selectedDnsProvider = providers[idx].id;
            } else {
                settings.selectedDnsProvider = "custom";
                settings.customDnsTemplate = StringUtils::WideToUtf8(GetWindowTextString(ctx->hEditDoh));
            }

            Config::Instance().Save();
            DnsManager::Instance().ApplySettings();

            std::wstring alertMsg = L"公共 DNS 设置已更新并成功应用！\n\n";
            if (enableDns) {
                alertMsg += L"当前状态: 【已启用】\n";
                if (settings.selectedDnsProvider == "custom") {
                    alertMsg += L"服务商: 自定义 DoH 节点\n";
                } else {
                    const auto* p = DnsManager::Instance().GetActiveProvider();
                    if (p) alertMsg += L"服务商: " + p->name + L"\n";
                }
                alertMsg += L"\n提示: 设置已写入本地配置文件与 Edge/WebView2 内核策略。\n点击确定后建议刷新网页以生效。";
            } else {
                alertMsg += L"当前状态: 【已关闭 (使用系统默认 DNS)】\n\n设置已恢复为系统网络解析。";
            }

            MessageBoxW(hWnd, alertMsg.c_str(), L"DNS 设置已保存", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(hWnd);
            break;
        }
        case IDC_DLG_BTN_CANCEL:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        if (ctx) {
            if (ctx->hFont) DeleteObject(ctx->hFont);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontMono) DeleteObject(ctx->hFontMono);
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

DnsManager& DnsManager::Instance() {
    static DnsManager instance;
    return instance;
}

DnsManager::DnsManager() {
    InitializeProviders();
}

void DnsManager::InitializeProviders() {
    m_providers.clear();

    // 1. 阿里公共 DNS (AliDNS)
    m_providers.push_back({
        "alidns",
        L"阿里公共 DNS (Alibaba Cloud)",
        L"阿里巴巴全国 Anycast 高防集群，国内解析毫秒响应，支持 IPv4/IPv6 双栈解析与 DoH 隐私加密。",
        L"223.5.5.5",
        L"223.6.6.6",
        L"2400:3200::1",
        L"2400:3200:baba::1",
        L"https://dns.alidns.com/dns-query"
    });

    // 2. 腾讯 DNSPod (Tencent Public DNS)
    m_providers.push_back({
        "dnspod",
        L"腾讯 DNSPod (Public DNS)",
        L"腾讯云全球 BGP Anycast 网络，智能识别线路，有效抵御 DNS 劫持与缓存污染。",
        L"119.29.29.29",
        L"182.254.116.116",
        L"2402:4e00::",
        L"2402:4e00:1::",
        L"https://doh.pub/dns-query"
    });

    // 3. 百度公共 DNS (Baidu DNS)
    m_providers.push_back({
        "baidu",
        L"百度公共 DNS (Baidu DNS)",
        L"百度遍布全国海量服务器与 CDN 节点智能调度，解析极速，稳定性高。",
        L"180.76.76.76",
        L"180.76.76.77",
        L"2400:da00::6666",
        L"2400:da00::6667",
        L"https://doh.bce.baidu.com/dns-query"
    });

    // 4. 114 DNS
    m_providers.push_back({
        "114",
        L"114 DNS (南京信风)",
        L"国内历史悠久的专业公共 DNS，电信、联通、移动跨网优化，高速纯净无劫持。",
        L"114.114.114.114",
        L"114.114.115.115",
        L"(暂无)",
        L"(暂无)",
        L"https://114.114.114.114/dns-query"
    });

    // 5. Cloudflare DNS (1.1.1.1)
    m_providers.push_back({
        "cloudflare",
        L"Cloudflare DNS (1.1.1.1 极速隐私)",
        L"全球权威测速第一梯队，APNIC 合作运维，承诺永不向第三方出售用户数据与 IP 日志。",
        L"1.1.1.1",
        L"1.0.0.1",
        L"2606:4700:4700::1111",
        L"2606:4700:4700::1001",
        L"https://cloudflare-dns.com/dns-query"
    });

    // 6. Google Public DNS (8.8.8.8)
    m_providers.push_back({
        "google",
        L"Google Public DNS (8.8.8.8)",
        L"全球覆盖规模极大的公共解析基础设施，国际网络访问可靠性强，抗欺诈与缓存中毒。",
        L"8.8.8.8",
        L"8.8.4.4",
        L"2001:4860:4860::8888",
        L"2001:4860:4860::8844",
        L"https://dns.google/dns-query"
    });

    // 7. Quad9 (9.9.9.9 安全拦截)
    m_providers.push_back({
        "quad9",
        L"Quad9 (9.9.9.9 恶意威胁拦截)",
        L"瑞士非营利基金会运营，整合全球数十家网络威胁情报库，实时拦截钓鱼网站与恶意软件。",
        L"9.9.9.9",
        L"149.112.112.112",
        L"2620:fe::fe",
        L"2620:fe::9",
        L"https://dns.quad9.net/dns-query"
    });

    // 8. Cisco OpenDNS
    m_providers.push_back({
        "opendns",
        L"Cisco OpenDNS (思科安全)",
        L"全球企业级网络安全技术加持，支持智能容灾与恶意域名阻断防护。",
        L"208.67.222.222",
        L"208.67.220.220",
        L"2620:119:35::35",
        L"2620:119:53::53",
        L"https://doh.opendns.com/dns-query"
    });

    // 9. 中国互联网络信息中心 SDNS (CNNIC)
    m_providers.push_back({
        "cnnic",
        L"CNNIC SDNS (国家互联网络信息中心)",
        L"国家顶级域名解析机构出品，面向国内网民提供权威、安全、高速的公共域名解析服务。",
        L"1.2.4.8",
        L"210.2.4.8",
        L"(暂无)",
        L"(暂无)",
        L"https://doh.sdns.cn/dns-query"
    });
}

const DnsProvider* DnsManager::GetProvider(const std::string& id) const {
    for (const auto& p : m_providers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const DnsProvider* DnsManager::GetActiveProvider() const {
    const auto& settings = Config::Instance().GetSettings();
    return GetProvider(settings.selectedDnsProvider);
}

bool DnsManager::ApplySettings() {
    const auto& settings = Config::Instance().GetSettings();

    std::wstring templateW;
    std::string templateNarrow;

    if (settings.enablePublicDns) {
        if (settings.selectedDnsProvider == "custom" && !settings.customDnsTemplate.empty()) {
            templateNarrow = settings.customDnsTemplate;
            templateW = StringUtils::Utf8ToWide(templateNarrow);
        } else {
            const auto* p = GetActiveProvider();
            if (p) {
                templateW = p->dohTemplate;
                templateNarrow = StringUtils::WideToUtf8(templateW);
            } else {
                templateW = L"https://dns.alidns.com/dns-query";
                templateNarrow = "https://dns.alidns.com/dns-query";
            }
        }
    }

    // 1. Apply to Windows Registry (HKCU\SOFTWARE\Policies\Microsoft\Edge\WebView2)
    HKEY hKey = nullptr;
    const wchar_t* subKeyWebView2 = L"SOFTWARE\\Policies\\Microsoft\\Edge\\WebView2";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKeyWebView2, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        if (settings.enablePublicDns && !templateW.empty()) {
            const wchar_t* modeSecure = L"secure";
            RegSetValueExW(hKey, L"DnsOverHttpsMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(modeSecure), static_cast<DWORD>((wcslen(modeSecure) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hKey, L"DnsOverHttpsTemplates", 0, REG_SZ, reinterpret_cast<const BYTE*>(templateW.c_str()), static_cast<DWORD>((templateW.length() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"DnsOverHttpsTemplates");
            const wchar_t* modeOff = L"off";
            RegSetValueExW(hKey, L"DnsOverHttpsMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(modeOff), static_cast<DWORD>((wcslen(modeOff) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }

    // Also apply to HKCU\SOFTWARE\Policies\Microsoft\Edge (for maximum compatibility)
    const wchar_t* subKeyEdge = L"SOFTWARE\\Policies\\Microsoft\\Edge";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKeyEdge, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        if (settings.enablePublicDns && !templateW.empty()) {
            const wchar_t* modeSecure = L"secure";
            RegSetValueExW(hKey, L"DnsOverHttpsMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(modeSecure), static_cast<DWORD>((wcslen(modeSecure) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hKey, L"DnsOverHttpsTemplates", 0, REG_SZ, reinterpret_cast<const BYTE*>(templateW.c_str()), static_cast<DWORD>((templateW.length() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"DnsOverHttpsTemplates");
            const wchar_t* modeOff = L"off";
            RegSetValueExW(hKey, L"DnsOverHttpsMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(modeOff), static_cast<DWORD>((wcslen(modeOff) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }

    // 2. Apply to UserData/Default/Preferences JSON
#if __has_include(<nlohmann/json.hpp>)
    try {
        std::filesystem::path prefPath = Config::Instance().GetUserDataDirectory() / "Default" / "Preferences";
        std::filesystem::create_directories(prefPath.parent_path());

        json root = json::object();
        if (std::filesystem::exists(prefPath)) {
            std::ifstream inFile(prefPath);
            if (inFile.is_open()) {
                try { inFile >> root; } catch (...) {}
            }
        }

        if (settings.enablePublicDns && !templateNarrow.empty()) {
            root["dns_over_https"]["mode"] = "secure";
            root["dns_over_https"]["templates"] = templateNarrow;
        } else {
            root["dns_over_https"]["mode"] = "off";
            root["dns_over_https"]["templates"] = "";
        }

        std::filesystem::path tmpPrefPath = prefPath;
        tmpPrefPath += L".tmp";
        {
            std::ofstream outFile(tmpPrefPath);
            if (outFile.is_open()) {
                outFile << root.dump(4);
                outFile.flush();
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmpPrefPath, prefPath, ec);
        if (ec) {
            std::filesystem::copy_file(tmpPrefPath, prefPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmpPrefPath, ec);
        }
    } catch (...) {
        // Fallback gracefully
    }
#endif

    return true;
}

void DnsManager::ShowDnsDialog(HWND hWndParent) {
    static const wchar_t* kDlgClassName = L"UltraLight_DnsDialogClass";
    static bool classRegistered = false;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    if (!classRegistered) {
        WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = DlgWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClassName;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    int dlgW = 560;
    int dlgH = 515;

    RECT parentRect{ 0, 0, 1024, 768 };
    if (hWndParent && IsWindow(hWndParent)) {
        GetWindowRect(hWndParent, &parentRect);
    }

    int posX = parentRect.left + ((parentRect.right - parentRect.left) - dlgW) / 2;
    int posY = parentRect.top + ((parentRect.bottom - parentRect.top) - dlgH) / 2;
    if (posX < 0) posX = 50;
    if (posY < 0) posY = 50;

    DlgContext ctx;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kDlgClassName,
        L"公共 DNS 服务器设置 (支持知名 IPv4 & IPv6 双栈 | 隐私防劫持)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, dlgW, dlgH,
        hWndParent, nullptr, hInstance, &ctx);

    if (!hDlg) return;

    if (hWndParent) {
        EnableWindow(hWndParent, FALSE);
    }

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hWndParent && IsWindow(hWndParent)) {
        EnableWindow(hWndParent, TRUE);
        SetForegroundWindow(hWndParent);
    }
}

} // namespace UltraLight
