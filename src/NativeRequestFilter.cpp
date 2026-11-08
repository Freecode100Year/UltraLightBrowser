#include "NativeRequestFilter.hpp"
#include "AdBlockRules.hpp"
#include "Config.hpp"
#include "ElementBlocker.hpp"
#include <shlwapi.h>
#include <cstring>

namespace UltraLight {

NativeRequestFilter& NativeRequestFilter::Instance() {
    static NativeRequestFilter s_instance;
    return s_instance;
}

// The rules themselves live in AdBlockRules.cpp (platform-independent and tested).
NativeRequestFilter::NativeRequestFilter() {
    m_enabled = Config::Instance().GetSettings().enableAdBlock;
}

const std::unordered_set<std::wstring>& NativeRequestFilter::GetBlockedDomainSet() const {
    return AdBlock::Rules::Default().Domains();
}

void NativeRequestFilter::SetEnabled(bool enabled) {
    m_enabled = enabled;
    auto& settings = Config::Instance().GetSettings();
    settings.enableAdBlock = enabled;
    Config::Instance().Save();
    // Element hiding follows the same switch.
    ElementBlocker::Instance().UpdateAllRulesScripts();
}

std::wstring NativeRequestFilter::ExtractHost(const std::wstring& uri) {
    return AdBlock::ExtractHost(uri);
}

std::wstring NativeRequestFilter::GetBaseDomain(const std::wstring& host) {
    return AdBlock::BaseDomain(host);
}

bool NativeRequestFilter::IsThirdParty(const std::wstring& reqHost, const std::wstring& topHost) {
    return AdBlock::IsThirdParty(reqHost, topHost);
}

void NativeRequestFilter::SetMainFrameNavigation(ICoreWebView2* webView, const std::wstring& uri) {
    std::lock_guard<std::mutex> lock(m_navMutex);
    auto& nav = m_nav[webView];
    nav.pendingUri = uri;
    nav.mainHost = ExtractHost(uri);
    nav.blocked = 0;
}

void NativeRequestFilter::ClearMainFrameNavigation(ICoreWebView2* webView) {
    std::lock_guard<std::mutex> lock(m_navMutex);
    const auto it = m_nav.find(webView);
    if (it != m_nav.end()) it->second.pendingUri.clear();
}

bool NativeRequestFilter::IsMainFrameNavigation(ICoreWebView2* webView, const std::wstring& uri) const {
    std::lock_guard<std::mutex> lock(m_navMutex);
    const auto it = m_nav.find(webView);
    if (it == m_nav.end() || it->second.pendingUri.empty()) return false;
    if (uri == it->second.pendingUri) return true;
    const std::wstring reqHost = ExtractHost(uri);
    return !it->second.mainHost.empty() && it->second.mainHost == reqHost;
}

std::wstring NativeRequestFilter::GetCurrentMainHost(ICoreWebView2* webView) const {
    std::lock_guard<std::mutex> lock(m_navMutex);
    const auto it = m_nav.find(webView);
    return it != m_nav.end() ? it->second.mainHost : std::wstring();
}

uint64_t NativeRequestFilter::GetBlockedCount(ICoreWebView2* webView) const {
    std::lock_guard<std::mutex> lock(m_navMutex);
    const auto it = m_nav.find(webView);
    return it != m_nav.end() ? it->second.blocked : 0;
}

void NativeRequestFilter::Unregister(ICoreWebView2* webView) {
    std::lock_guard<std::mutex> lock(m_navMutex);
    m_nav.erase(webView);
}

bool NativeRequestFilter::ShouldBlock(const std::wstring& uri, const std::wstring& topHost) const {
    if (!m_enabled) return false;
    return AdBlock::Rules::Default().Evaluate(uri, topHost, AdBlock::Resource::Other).block;
}

void NativeRequestFilter::Initialize(ICoreWebView2* webView, ICoreWebView2Environment* environment) {
    if (!webView || !environment) return;
    m_environment = environment;

    // Only requests that can possibly be blocked are routed to the browser: every
    // intercepted request costs a round trip to the UI thread, so a catch-all "*"
    // filter slowed down every page load. One pattern per domain covers its
    // subdomains too; the handler still applies the exact rules.
    // Where supported, requests made by workers (service/shared/dedicated) are
    // routed too, so trackers cannot slip out through a service worker.
    wil::com_ptr<ICoreWebView2_22> webView22;
    webView->QueryInterface(IID_PPV_ARGS(&webView22));
    for (const auto& pattern : AdBlock::Rules::Default().RequestFilterPatterns()) {
        if (!webView22 || FAILED(webView22->AddWebResourceRequestedFilterWithRequestSourceKinds(
                pattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
                COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL))) {
            webView->AddWebResourceRequestedFilter(pattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
    }

    webView->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                return this->HandleWebResourceRequested(sender, args);
            }
        ).Get(),
        nullptr
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

    // The main frame document navigation is always allowed.
    if (context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT && IsMainFrameNavigation(sender, uri.get())) {
        return S_OK;
    }

    std::wstring topHost = GetCurrentMainHost(sender);
    if (topHost.empty() && sender) {
        wil::unique_cotaskmem_string topUri;
        if (SUCCEEDED(sender->get_Source(&topUri)) && topUri.get()) {
            topHost = ExtractHost(topUri.get());
        }
    }
    // Per-site exception ("此网站的设置 → 内容拦截器" off).
    if (!topHost.empty() && m_siteAllowsAds && m_siteAllowsAds(topHost)) {
        return S_OK;
    }

    AdBlock::Resource kind = AdBlock::Resource::Other;
    switch (context) {
    case COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT: kind = AdBlock::Resource::Document; break;
    case COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE: kind = AdBlock::Resource::Image; break;
    case COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT: kind = AdBlock::Resource::Script; break;
    default: break;
    }

    const AdBlock::Decision decision = AdBlock::Rules::Default().Evaluate(uri.get(), topHost, kind);
    if (!decision.block) return S_OK;

    // Blocked scripts with a known API get a harmless stand-in so the page keeps
    // working (link clicks waiting on ga() callbacks, googletag.cmd queues, ...).
    // Everything else gets an empty 204 response.
    wil::com_ptr<IStream> body;
    const wchar_t* status = L"No Content";
    int code = 204;
    const wchar_t* headers = L"Content-Type: text/plain\r\nCache-Control: no-store";
    if (decision.surrogate) {
        body.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(decision.surrogate),
                                      static_cast<UINT>(std::strlen(decision.surrogate))));
        code = 200;
        status = L"OK";
        headers = L"Content-Type: text/javascript; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store";
    } else {
        body.attach(SHCreateMemStream(nullptr, 0));
    }

    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
    const HRESULT hr = m_environment->CreateWebResourceResponse(body.get(), code, status, headers, &response);
    if (SUCCEEDED(hr) && response) {
        args->put_Response(response.get());
        m_blockedCount.fetch_add(1);
        std::lock_guard<std::mutex> lock(m_navMutex);
        const auto it = m_nav.find(sender);
        if (it != m_nav.end()) ++it->second.blocked;
    }
    return S_OK;
}

} // namespace UltraLight
