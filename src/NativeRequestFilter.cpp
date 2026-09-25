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

    // Fast domain suffix match list (ad networks, telemetry, trackers, cryptominers)
    m_blockedDomains = {
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
        L"analytics.tiktok.com"
    };

    // Fast keyword substring matches in URL
    m_blockedKeywords = {
        L"/pagead/js/",
        L"/pagead/show_ads.js",
        L"/advert.js",
        L"/ad.js",
        L"/ads.js",
        L"/google-analytics.com/analytics.js",
        L"/gtag/js?id=",
        L"/gtm.js?id=",
        L"/hm.js?",
        L"/beacon.js"
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

    size_t hostStart = protoEnd + 3;
    size_t hostEnd = uri.find_first_of(L"/ :?#", hostStart);
    if (hostEnd == std::wstring::npos) {
        hostEnd = uri.length();
    }

    std::wstring host = uri.substr(hostStart, hostEnd - hostStart);
    std::transform(host.begin(), host.end(), host.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return host;
}

bool NativeRequestFilter::ShouldBlock(const std::wstring& uri) {
    if (!m_enabled || uri.empty()) return false;

    std::wstring lowerUri = uri;
    std::transform(lowerUri.begin(), lowerUri.end(), lowerUri.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });

    // Check host suffix match
    std::wstring host = ExtractHost(lowerUri);
    if (!host.empty()) {
        for (const auto& domain : m_blockedDomains) {
            if (host == domain) return true;
            if (host.length() > domain.length() &&
                host.rfind(L'.' + domain) == (host.length() - domain.length() - 1)) {
                return true;
            }
        }
    }

    // Check path keywords
    for (const auto& kw : m_blockedKeywords) {
        if (lowerUri.find(kw) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

void NativeRequestFilter::Initialize(ICoreWebView2* webView, ICoreWebView2Environment* environment) {
    if (!webView || !environment) return;
    m_environment = environment;

    // Intercept all resource contexts (script, image, stylesheet, xhr, fetch, subframe)
    webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    webView->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                return this->HandleWebResourceRequested(sender, args);
            }
        ).Get(),
        &m_resourceRequestedToken
    );
}

HRESULT NativeRequestFilter::HandleWebResourceRequested(ICoreWebView2* /*sender*/, ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!m_enabled || !args || !m_environment) return S_OK;

    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return S_OK;

    wil::unique_cotaskmem_string uri;
    if (FAILED(request->get_Uri(&uri)) || !uri.get()) return S_OK;

    if (ShouldBlock(uri.get())) {
        wil::com_ptr<IStream> emptyStream;
        emptyStream.attach(SHCreateMemStream(nullptr, 0));
        wil::com_ptr<ICoreWebView2WebResourceResponse> response;

        HRESULT hr = m_environment->CreateWebResourceResponse(
            emptyStream.get(),
            204,
            L"No Content",
            L"Content-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: max-age=86400\r\n",
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
