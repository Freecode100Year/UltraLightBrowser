#include "ElementBlocker.hpp"
#include "Config.hpp"
#include <urlmon.h>
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
            return std::string(hostW.begin(), hostW.end());
        }
    }
    return "";
}

void ElementBlocker::Initialize(ICoreWebView2* webView) {
    if (!webView) return;

    // Default pre-render injection handler
    std::wstring initScript = LR"(
        window.__ultralight_blocker_ready = true;
    )";
    webView->AddScriptToExecuteOnDocumentCreated(initScript.c_str(), nullptr);
}

void ElementBlocker::OnNavigationStarting(ICoreWebView2* webView, const std::wstring& uri) {
    if (!webView) return;

    std::string host = ExtractHostFromUri(uri);
    if (host.empty()) return;

    std::string cssRules = Config::Instance().GetBlockRulesForHost(host);
    if (cssRules.empty()) return;

    // Escape rules for JS string literal
    std::string escapedRules;
    for (char c : cssRules) {
        if (c == '\"') escapedRules += "\\\"";
        else if (c == '\\') escapedRules += "\\\\";
        else if (c == '\n') escapedRules += "\\n";
        else if (c == '\r') escapedRules += "\\r";
        else escapedRules += c;
    }

    std::wstring jsCode = LR"(
        (function() {
            const rules = ")" + std::wstring(escapedRules.begin(), escapedRules.end()) + LR"(";
            if (!rules) return;
            const style = document.createElement('style');
            style.id = '__ultralight_blocker_css__';
            style.textContent = rules;
            (document.head || document.documentElement).appendChild(style);
        })();
    )";

    webView->AddScriptToExecuteOnDocumentCreated(jsCode.c_str(), nullptr);
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
                        window.chrome.webview.postMessage(JSON.stringify({
                            type: 'ELEMENT_PICKED',
                            selector: selector,
                            host: window.location.hostname
                        }));
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

bool ElementBlocker::HandleWebMessage(const std::wstring& messageJson) {
#if __has_include(<nlohmann/json.hpp>)
    try {
        std::string narrowMsg(messageJson.begin(), messageJson.end());
        json data = json::parse(narrowMsg);

        if (data.contains("type") && data["type"] == "ELEMENT_PICKED") {
            std::string host = data["host"].get<std::string>();
            std::string selector = data["selector"].get<std::string>();

            Config::Instance().AddBlockRule(host, selector);
            m_pickerActive = false;
            return true;
        }
    } catch (...) {
        // Ignored or logged
    }
#endif
    return false;
}

} // namespace UltraLight
