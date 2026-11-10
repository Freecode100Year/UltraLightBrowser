#pragma once

// C++ bridge to the platform-independent Rust ad/tracker rule engine. The bridge
// preserves the WebView2-facing API; CosmeticScript() remains C++ because it
// embeds page scripts. Rust rules and the production bridge are tested on Linux.

#include <string>
#include <unordered_set>
#include <vector>

namespace UltraLight::AdBlock {

enum class Resource { Document, Image, Script, Other };

struct Decision {
    bool block = false;
    // When non-null, answer with this JavaScript (HTTP 200) instead of an empty
    // response: pages that call ga()/gtag()/googletag/adsbygoogle keep working.
    const char* surrogate = nullptr;
};

// Hostname of an absolute URL: lowercase, no userinfo, port or trailing dots.
// IPv6 literals keep their brackets. Empty for URLs without "://".
std::wstring ExtractHost(const std::wstring& uri);

// Registrable domain ("news.bbc.co.uk" -> "bbc.co.uk", "a.github.io" stays).
std::wstring BaseDomain(const std::wstring& host);
bool IsThirdParty(const std::wstring& reqHost, const std::wstring& topHost);

class Rules {
public:
    static const Rules& Default();

    // Blocklist entry that covers host (itself or a parent domain), or nullptr.
    const std::wstring* MatchHost(const std::wstring& host) const;

    // Full decision for one request made by a page whose top-level host is topHost.
    Decision Evaluate(const std::wstring& uri, const std::wstring& topHost, Resource kind) const;

    // Minimal set of WebView2 request filter patterns that routes every request
    // Evaluate() may block (patterns may over-match; Evaluate is authoritative).
    std::vector<std::wstring> RequestFilterPatterns() const;

    const std::unordered_set<std::wstring>& Domains() const { return m_domains; }

private:
    Rules();

    std::unordered_set<std::wstring> m_domains;
};

// Self-contained page script: generic and site-specific element hiding plus the
// YouTube ad scriptlet. It stays inert when blocking is off, or when the top-level
// site is in disabledHosts ("此网站的设置 → 内容拦截器" off).
std::wstring CosmeticScript(bool enabled, const std::vector<std::wstring>& disabledHosts);

} // namespace UltraLight::AdBlock
