#include "ElementBlocker.hpp"
#include "Config.hpp"
#include "StringUtils.hpp"
#include <urlmon.h>
#include <wininet.h>
#include <shlwapi.h>
#include <sstream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

namespace UltraLight {

ElementBlocker& ElementBlocker::Instance() {
    static ElementBlocker s_instance;
    return s_instance;
}

std::string ElementBlocker::ExtractHostFromUri(const std::wstring& uri) {
    URL_COMPONENTSW urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = 1;

    if (InternetCrackUrlW(uri.c_str(), static_cast<DWORD>(uri.length()), 0, &urlComp)) {
        if (urlComp.lpszHostName && urlComp.dwHostNameLength > 0) {
            std::wstring hostW(urlComp.lpszHostName, urlComp.dwHostNameLength);
            return StringUtils::WideToUtf8(hostW);
        }
    }
    return "";
}

void ElementBlocker::Initialize(ICoreWebView2* webView) {
    if (!webView) return;
    m_webView = webView;
    UpdateRulesScript(webView);
}

void ElementBlocker::UpdateRulesScript(ICoreWebView2* webView) {
    if (!webView) return;

    // Remove previously registered script to prevent accumulation across updates
    if (!m_injectedScriptId.empty()) {
        webView->RemoveScriptToExecuteOnDocumentCreated(m_injectedScriptId.c_str());
        m_injectedScriptId.clear();
    }

#if __has_include(<nlohmann/json.hpp>)
    auto allRules = Config::Instance().GetAllBlockRules();
    json rulesObj = json::object();
    for (const auto& [host, selectors] : allRules) {
        if (!selectors.empty()) {
            rulesObj[host] = selectors;
        }
    }

    std::string rulesJsonStr = rulesObj.dump();
    std::wstring rulesJsonW = StringUtils::Utf8ToWide(rulesJsonStr);

    std::wstring initScript = LR"(
        (function() {
            if (window.__ultralight_blocker_injected) return;
            window.__ultralight_blocker_injected = true;

            const rulesMap = )" + rulesJsonW + LR"(;

            function applyRules() {
                try {
                    const host = window.location.hostname;
                    if (!host) return;

                    let selectors = [];
                    for (const domain in rulesMap) {
                        if (host === domain || host.endsWith('.' + domain)) {
                            const arr = rulesMap[domain];
                            if (Array.isArray(arr)) {
                                selectors = selectors.concat(arr);
                            }
                        }
                    }

                    if (!selectors.length) return;

                    let style = document.getElementById('__ultralight_blocker_css__');
                    if (!style) {
                        style = document.createElement('style');
                        style.id = '__ultralight_blocker_css__';
                        (document.head || document.documentElement).appendChild(style);
                    }
                    style.textContent = selectors.map(function(s) { return s + ' { display: none !important; }'; }).join('\n');
                } catch(e) {}
            }

            if (document.head || document.documentElement) {
                applyRules();
            } else {
                document.addEventListener('DOMContentLoaded', applyRules, { once: true });
            }
        })();
    )";

    webView->AddScriptToExecuteOnDocumentCreated(
        initScript.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [this](HRESULT hr, LPCWSTR id) -> HRESULT {
                if (SUCCEEDED(hr) && id) {
                    m_injectedScriptId = id;
                }
                return S_OK;
            }
        ).Get()
    );
#endif
}

void ElementBlocker::OnNavigationStarting(ICoreWebView2* /*webView*/, const std::wstring& uri) {
    m_currentHost = StringUtils::Utf8ToWide(ExtractHostFromUri(uri));
}

void ElementBlocker::TogglePickerMode(ICoreWebView2* webView) {
    if (!webView) return;

    m_pickerActive = !m_pickerActive;

    std::wstring pickerJs = LR"(
        (function() {
            if (window.__ultralight_picker_active) {
                if (window.__ultralight_picker_cleanup) window.__ultralight_picker_cleanup();
                return;
            }
            window.__ultralight_picker_active = true;
            let lastEl = null;

            function getCssPath(el) {
                if (!(el instanceof Element)) return '';
                let path = [];
                while (el && el.nodeType === Node.ELEMENT_NODE) {
                    let selector = el.nodeName.toLowerCase();
                    if (el.id) {
                        selector += '#' + el.id;
                        path.unshift(selector);
                        break;
                    } else {
                        let sib = el, nth = 1;
                        while (sib = sib.previousElementSibling) {
                            if (sib.nodeName.toLowerCase() === selector) nth++;
                        }
                        if (nth !== 1) selector += ':nth-of-type(' + nth + ')';
                    }
                    path.unshift(selector);
                    el = el.parentNode;
                }
                return path.join(' > ');
            }

            function onMouseMove(e) {
                if (lastEl && lastEl !== e.target) {
                    lastEl.style.outline = '';
                }
                lastEl = e.target;
                if (lastEl) {
                    lastEl.style.outline = '2px solid red';
                    lastEl.style.cursor = 'crosshair';
                }
            }

            function onClick(e) {
                e.preventDefault();
                e.stopPropagation();
                if (lastEl) {
                    lastEl.style.outline = '';
                    const selector = getCssPath(lastEl);
                    if (window.chrome && window.chrome.webview) {
                        window.chrome.webview.postMessage({
                            type: 'ELEMENT_PICKED',
                            selector: selector,
                            host: window.location.hostname
                        });
                    }
                    lastEl.style.display = 'none';
                }
                cleanup();
            }

            function cleanup() {
                if (lastEl) lastEl.style.outline = '';
                window.removeEventListener('mousemove', onMouseMove, true);
                window.removeEventListener('click', onClick, true);
                window.__ultralight_picker_active = false;
            }

            window.__ultralight_picker_cleanup = cleanup;
            window.addEventListener('mousemove', onMouseMove, true);
            window.addEventListener('click', onClick, true);
        })();
    )";

    webView->ExecuteScript(pickerJs.c_str(), nullptr);
}

bool ElementBlocker::HandleWebMessage(const std::wstring& messageJson, const std::wstring& sourceUri) {
#if __has_include(<nlohmann/json.hpp>)
    // Guard against malicious web pages spoofing messages when picker mode is inactive
    if (!m_pickerActive) return false;

    try {
        std::string narrowMsg = StringUtils::WideToUtf8(messageJson);
        json data = json::parse(narrowMsg);

        // Handle case where web message was serialized as string
        if (data.is_string()) {
            data = json::parse(data.get<std::string>());
        }

        if (data.is_object() && data.contains("type") && data["type"] == "ELEMENT_PICKED") {
            std::string verifiedHost = "";
            if (!sourceUri.empty()) {
                verifiedHost = ExtractHostFromUri(sourceUri);
            }
            if (verifiedHost.empty() && data.contains("host")) {
                verifiedHost = data["host"].get<std::string>();
            }
            if (verifiedHost.empty()) return false;

            std::string selector = data["selector"].get<std::string>();
            if (selector.empty() || selector.length() > 4096) return false;

            Config::Instance().AddBlockRule(verifiedHost, selector);
            m_pickerActive = false;

            if (m_webView) {
                UpdateRulesScript(m_webView);

                // Instantly apply the block rule on currently loaded document
                std::wstring hideNowJs = L"(function(){ const s = document.createElement('style'); s.textContent = '"
                    + StringUtils::Utf8ToWide(selector) + L" { display: none !important; }'; (document.head||document.documentElement).appendChild(s); })();";
                m_webView->ExecuteScript(hideNowJs.c_str(), nullptr);
            }
            return true;
        }
    } catch (...) {
        // Ignored or logged
    }
#endif
    return false;
}

} // namespace UltraLight
