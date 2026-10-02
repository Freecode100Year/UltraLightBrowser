#include "NativeRequestFilter.hpp"
#include "Config.hpp"
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace UltraLight {

NativeRequestFilter& NativeRequestFilter::Instance() {
    static NativeRequestFilter s_instance;
    return s_instance;
}

NativeRequestFilter::NativeRequestFilter() {
    m_enabled = Config::Instance().GetSettings().enableAdBlock;

    // Fast domain match set (ad networks, trackers, telemetry)
    // Sentry.io and Bugsnag.com removed per security/stability audit
    m_blockedDomainSet = {
        // Google Ads & Analytics
        L"doubleclick.net",
        L"google-analytics.com",
        L"googletagmanager.com",
        L"googlesyndication.com",
        L"adservice.google.com",
        L"pagead2.googlesyndication.com",
        L"partner.googleadservices.com",
        // Baidu Ads & Tracking
        L"pos.baidu.com",
        L"cpro.baidu.com",
        L"hm.baidu.com",
        L"dup.baidustatic.com",
        L"cm.baidu.com",
        L"sp1.baidu.com",
        // Tencent & WeChat Ads
        L"e.qq.com",
        L"gdt.qq.com",
        L"pgdt.gtimg.cn",
        L"tajs.qq.com",
        L"tmead.qq.com",
        // Alibaba Ad / Tracking
        L"alimama.com",
        L"tanx.com",
        L"ac.mmstat.com",
        L"atanx.alicdn.com",
        // Chinese Telemetry / Trackers
        L"cnzz.com",
        L"cnzz.net",
        L"umeng.com",
        L"umengcloud.com",
        L"statcounter.com",
        L"adpushup.com",
        L"51.la",
        L"miaozhen.com",
        L"growingio.com",
        // Global Ad Networks & Telemetry
        L"scorecardresearch.com",
        L"adnxs.com",
        L"criteo.com",
        L"outbrain.com",
        L"taboola.com",
        L"amazon-adsystem.com",
        L"rubiconproject.com",
        L"casalemedia.com",
        L"pubmatic.com",
        L"openx.net",
        L"smartadserver.com",
        L"hotjar.com",
        L"clarity.ms",
        L"segment.io",
        L"mixpanel.com",
        L"adroll.com",
        L"yieldmo.com",
        L"ads-twitter.com",
        L"ads.tiktok.com",
        L"analytics.tiktok.com",
        L"connect.facebook.net",
        L"amplitude.com",
        L"log.byteoversea.com",
        L"sensorsdata.cn",
        L"app-measurement.com",
        L"branch.io",
        L"adjust.com",
        L"appsflyer.com"
    };

    // Specific ad/tracker path keywords (overbroad keywords removed)
    m_blockedKeywords = {
        L"/pagead/js/",
        L"/pagead/show_ads.js",
        L"/advert.js",
        L"/ads.js",
        L"/google-analytics.com/analytics.js",
        L"/gtag/js?id=",
        L"/gtm.js?id=",
        L"/hm.js?",
        L"/beacon.js",
        L"/pixel.gif",
        L"/pixel.png"
    };
}

void NativeRequestFilter::SetEnabled(bool enabled) {
    m_enabled = enabled;
    auto& settings = Config::Instance().GetSettings();
    settings.enableAdBlock = enabled;
    Config::Instance().Save();
}

std::wstring NativeRequestFilter::ExtractHost(const std::wstring& uri) {
    size_t protoEnd = uri.find(L"://");
    if (protoEnd == std::wstring::npos) return L"";

    size_t authStart = protoEnd + 3;
    // Authority ends at the first '/', '?', or '#'
    size_t authEnd = uri.find_first_of(L"/?#", authStart);
    if (authEnd == std::wstring::npos) {
        authEnd = uri.length();
    }
    if (authStart >= authEnd) return L"";

    // Handle userinfo (e.g. https://user:pass@example.com/)
    size_t hostStart = authStart;
    size_t atPos = uri.rfind(L'@', authEnd);
    if (atPos != std::wstring::npos && atPos >= authStart) {
        hostStart = atPos + 1;
    }
    if (hostStart >= authEnd) return L"";

    std::wstring host;
    // Handle IPv6 literal [2001:db8::1]
    if (uri[hostStart] == L'[') {
        size_t closeBracket = uri.find(L']', hostStart);
        if (closeBracket != std::wstring::npos && closeBracket < authEnd) {
            host = uri.substr(hostStart, closeBracket - hostStart + 1);
        } else {
            host = uri.substr(hostStart, authEnd - hostStart);
        }
    } else {
        // Strip port (e.g. example.com:8080)
        size_t colonPos = uri.find(L':', hostStart);
        size_t hostEnd = (colonPos != std::wstring::npos && colonPos < authEnd) ? colonPos : authEnd;
        host = uri.substr(hostStart, hostEnd - hostStart);
    }

    // Strip trailing dots (e.g. doubleclick.net. or FQDN trailing dot)
    while (!host.empty() && host.back() == L'.') {
        host.pop_back();
    }

    // Lowercase host only
    for (auto& ch : host) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return host;
}

std::wstring NativeRequestFilter::GetBaseDomain(const std::wstring& host) {
    if (host.empty()) return L"";

    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < host.length()) {
        size_t dot = host.find(L'.', start);
        if (dot == std::wstring::npos) {
            parts.push_back(host.substr(start));
            break;
        }
        parts.push_back(host.substr(start, dot - start));
        start = dot + 1;
    }

    if (parts.size() <= 2) {
        return host;
    }

    // Check common two-level ccTLDs (e.g. .com.cn, .co.uk, .org.cn, etc.)
    const auto& tld = parts[parts.size() - 1];
    const auto& sld = parts[parts.size() - 2];
    bool isSecondLevelCctld = false;
    if (tld.length() == 2 && (sld == L"com" || sld == L"net" || sld == L"org" || sld == L"gov" || sld == L"edu" || sld == L"co")) {
        isSecondLevelCctld = true;
    }

    size_t takeParts = isSecondLevelCctld ? 3 : 2;
    if (parts.size() < takeParts) return host;

    std::wstring baseDomain;
    for (size_t i = parts.size() - takeParts; i < parts.size(); ++i) {
        if (!baseDomain.empty()) baseDomain += L".";
        baseDomain += parts[i];
    }
    return baseDomain;
}

bool NativeRequestFilter::IsThirdParty(const std::wstring& reqHost, const std::wstring& topHost) {
    if (reqHost.empty() || topHost.empty()) return true;
    if (reqHost == topHost) return false;
    return GetBaseDomain(reqHost) != GetBaseDomain(topHost);
}

bool NativeRequestFilter::ShouldBlock(const std::wstring& uri) {
    if (!m_enabled || uri.empty()) return false;

    // 1. Fast O(1) domain & parent subdomain matching
    std::wstring host = ExtractHost(uri);
    if (!host.empty()) {
        if (m_blockedDomainSet.find(host) != m_blockedDomainSet.end()) {
            return true;
        }

        size_t dotPos = host.find(L'.');
        while (dotPos != std::wstring::npos) {
            std::wstring parentDomain = host.substr(dotPos + 1);
            if (m_blockedDomainSet.find(parentDomain) != m_blockedDomainSet.end()) {
                return true;
            }
            dotPos = host.find(L'.', dotPos + 1);
        }
    }

    // 2. Path keyword matching (targeted patterns only, no full-URI lowercase copy)
    size_t protoEnd = uri.find(L"://");
    size_t pathStart = (protoEnd != std::wstring::npos) ? uri.find(L'/', protoEnd + 3) : uri.find(L'/');
    if (pathStart != std::wstring::npos) {
        std::wstring path = uri.substr(pathStart);
        for (auto& ch : path) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        for (const auto& kw : m_blockedKeywords) {
            if (path.find(kw) != std::wstring::npos) {
                return true;
            }
        }
    }

    return false;
}

void NativeRequestFilter::Initialize(ICoreWebView2* webView, ICoreWebView2Environment* environment) {
    if (!webView || !environment) return;
    m_environment = environment;

    // Register request filters (scripts, documents/iframes, images, XHR, fetch, ping, other)
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_XML_HTTP_REQUEST);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FETCH);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_PING);
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER);

    webView->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                return this->HandleWebResourceRequested(sender, args);
            }
        ).Get(),
        &m_resourceRequestedToken
    );
}

HRESULT NativeRequestFilter::HandleWebResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!m_enabled || !args || !m_environment) return S_OK;

    COREWEBVIEW2_WEB_RESOURCE_CONTEXT context;
    if (FAILED(args->get_ResourceContext(&context))) {
        context = COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER;
    }

    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return S_OK;

    wil::unique_cotaskmem_string uri;
    if (FAILED(request->get_Uri(&uri)) || !uri.get()) return S_OK;

    // For documents (subframes/iframes) and images (tracking pixels), only inspect third-party requests
    if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE || context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT) {
        if (sender) {
            wil::unique_cotaskmem_string topUri;
            if (SUCCEEDED(sender->get_Source(&topUri)) && topUri.get()) {
                std::wstring topHost = ExtractHost(topUri.get());
                std::wstring reqHost = ExtractHost(uri.get());
                if (!topHost.empty() && !reqHost.empty() && !IsThirdParty(reqHost, topHost)) {
                    // First-party subframe or image: allow directly without blocking
                    return S_OK;
                }
            }
        }
    }

    if (ShouldBlock(uri.get())) {
        wil::com_ptr<IStream> emptyStream;
        emptyStream.attach(SHCreateMemStream(nullptr, 0));
        wil::com_ptr<ICoreWebView2WebResourceResponse> response;

        HRESULT hr = m_environment->CreateWebResourceResponse(
            emptyStream.get(),
            204,
            L"No Content",
            L"Content-Type: text/plain\r\n",
            &response
        );
        if (SUCCEEDED(hr) && response) {
            args->put_Response(response.get());
            m_blockedCount.fetch_add(1);
        }
    }

    return S_OK;
}

} // namespace UltraLight
