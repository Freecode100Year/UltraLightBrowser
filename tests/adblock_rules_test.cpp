// Tests the production ad/tracker rules (src/AdBlockRules.cpp) without Windows.
//   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/adblock_rules_test.cpp src/AdBlockRules.cpp -o /tmp/adblock-tests
#include "AdBlockRules.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace UltraLight::AdBlock;

static int g_failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

static bool Blocks(const wchar_t* uri, const wchar_t* top, Resource kind = Resource::Script) {
    return Rules::Default().Evaluate(uri, top, kind).block;
}

static const char* Surrogate(const wchar_t* uri, const wchar_t* top) {
    return Rules::Default().Evaluate(uri, top, Resource::Script).surrogate;
}

static bool Matches(const std::wstring& pattern, const std::wstring& text) {
    // WebView2 filter semantics: '*' matches any run of characters.
    size_t p = 0, t = 0, star = std::wstring::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == L'*') { star = p++; mark = t; }
        else if (star != std::wstring::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

static bool Routed(const std::wstring& uri) {
    for (const auto& p : Rules::Default().RequestFilterPatterns()) {
        if (Matches(p, uri)) return true;
    }
    return false;
}

static void TestHosts() {
    CHECK(ExtractHost(L"https://User:Pw@WWW.DoubleClick.NET.:443/x?y#z") == L"www.doubleclick.net");
    CHECK(ExtractHost(L"https://[2001:db8::1]:8080/") == L"[2001:db8::1]");
    CHECK(ExtractHost(L"https://example.com?q=a@b.com") == L"example.com");
    CHECK(ExtractHost(L"https://example.com#a@b") == L"example.com");
    CHECK(ExtractHost(L"about:blank").empty());
    CHECK(ExtractHost(L"https:///path").empty());

    CHECK(BaseDomain(L"news.bbc.co.uk") == L"bbc.co.uk");
    CHECK(BaseDomain(L"a.b.example.com") == L"example.com");
    CHECK(BaseDomain(L"user.github.io") == L"user.github.io");
    CHECK(BaseDomain(L"x.user.github.io") == L"user.github.io");
    CHECK(BaseDomain(L"www.baidu.com.cn") == L"baidu.com.cn");
    CHECK(BaseDomain(L"192.168.1.20") == L"192.168.1.20");
    CHECK(IsThirdParty(L"10.0.0.1", L"10.0.0.2"));
    CHECK(!IsThirdParty(L"static.example.com", L"www.example.com"));
    CHECK(IsThirdParty(L"a.github.io", L"b.github.io"));
}

static void TestDomainRules() {
    const auto& r = Rules::Default();
    CHECK(r.MatchHost(L"doubleclick.net") != nullptr);
    CHECK(r.MatchHost(L"securepubads.g.doubleclick.net") != nullptr);
    CHECK(r.MatchHost(L"notdoubleclick.net") == nullptr);
    CHECK(r.MatchHost(L"baidu.com") == nullptr);
    CHECK(r.MatchHost(L"www.google.com") == nullptr);

    CHECK(Blocks(L"https://www.google-analytics.com/collect?v=2", L"news.example.com", Resource::Other));
    CHECK(Blocks(L"https://hm.baidu.com/hm.gif?x", L"www.example.cn", Resource::Image));
    CHECK(Blocks(L"https://cdn.taboola.com/libtrc/x/loader.js", L"news.example.com"));
    CHECK(!Blocks(L"https://www.example.com/app.js", L"www.example.com"));
    CHECK(!Blocks(L"data:text/javascript,1", L"www.example.com"));
    CHECK(!Blocks(L"blob:https://www.example.com/uuid", L"www.example.com"));

    // Same-site trackers on a tracking subdomain are still blocked...
    CHECK(Blocks(L"https://analytics.tiktok.com/i18n/pixel/events.js", L"www.tiktok.com"));
    CHECK(Blocks(L"https://hm.baidu.com/hm.js?abc", L"www.baidu.com"));
    // ...but a listed vendor's own site keeps working.
    CHECK(!Blocks(L"https://cdn.mixpanel.com/app.js", L"mixpanel.com"));
    CHECK(!Blocks(L"https://static.hotjar.com/app.js", L"www.hotjar.com"));
    CHECK(!Blocks(L"https://www.taboola.com/assets/site.css", L"www.taboola.com", Resource::Other));
}

static void TestFirstPartyExemptions() {
    // First-party images and frames are never blocked, even on keyword paths.
    CHECK(!Blocks(L"https://www.example.com/img/pixel.gif", L"www.example.com", Resource::Image));
    CHECK(!Blocks(L"https://shop.example.com/embed", L"www.example.com", Resource::Document));
    CHECK(Blocks(L"https://tracker.other.com/pixel.gif?id=1", L"www.example.com", Resource::Image));
}

static void TestPathRules() {
    CHECK(Blocks(L"https://www.facebook.com/tr?id=1&ev=PageView", L"shop.example.com", Resource::Image));
    CHECK(Blocks(L"https://www.facebook.com/tr/?id=1", L"shop.example.com", Resource::Image));
    CHECK(!Blocks(L"https://www.facebook.com/translations/x", L"shop.example.com", Resource::Other));
    CHECK(!Blocks(L"https://www.facebook.com/plugins/like.php", L"shop.example.com", Resource::Document));
    CHECK(Blocks(L"https://www.google.com/pagead/1p-conversion/123/", L"shop.example.com", Resource::Image));
    CHECK(!Blocks(L"https://www.google.com/recaptcha/api.js", L"shop.example.com"));
    CHECK(Blocks(L"https://www.youtube.com/api/stats/ads?ver=2", L"www.youtube.com", Resource::Other));
    CHECK(Blocks(L"https://www.youtube.com/pagead/viewthroughconversion/1", L"www.youtube.com", Resource::Other));
    CHECK(!Blocks(L"https://www.youtube.com/api/stats/watchtime?x", L"www.youtube.com", Resource::Other));
    CHECK(!Blocks(L"https://www.youtube.com/youtubei/v1/player", L"www.youtube.com", Resource::Other));
    CHECK(Blocks(L"https://t.co/i/adsct?txn_id=1", L"shop.example.com", Resource::Image));
    CHECK(Blocks(L"https://example.org/static/ADS.JS", L"www.example.com"));
    CHECK(!Blocks(L"https://example.org/static/uploads.js", L"www.example.com"));
    CHECK(!Blocks(L"https://example.org/page#/ads.js", L"www.example.com"));
}

static void TestSurrogates() {
    CHECK(Surrogate(L"https://www.google-analytics.com/analytics.js", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://www.googletagmanager.com/gtag/js?id=G-1", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://www.googletagmanager.com/gtm.js?id=GTM-1", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://securepubads.g.doubleclick.net/tag/js/gpt.js", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://www.googletagservices.com/tag/js/gpt.js", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js?client=1", L"a.com") != nullptr);
    CHECK(Surrogate(L"https://www.google-analytics.com/g/collect", L"a.com") == nullptr);
    // Surrogates answer script loads only, never beacons or images.
    CHECK(Rules::Default().Evaluate(L"https://www.google-analytics.com/analytics.js", L"a.com", Resource::Other).surrogate == nullptr);
}

static void TestFilterPatterns() {
    const auto patterns = Rules::Default().RequestFilterPatterns();
    // Covered subdomain entries need no filter of their own.
    for (const auto& p : patterns) {
        CHECK(p != L"*://*mobads-logs.baidu.com/*" || Rules::Default().MatchHost(L"baidu.com") == nullptr);
        CHECK(p != L"*://*partner.googleadservices.com/*");
        CHECK(p != L"*://*securepubads.g.doubleclick.net/*");
    }
    CHECK(patterns.size() < Rules::Default().Domains().size() + 30);

    // Every request Evaluate blocks must be routed to the handler.
    const wchar_t* samples[] = {
        L"https://doubleclick.net/x", L"https://ad.doubleclick.net/x", L"https://hm.baidu.com/hm.js?1",
        L"https://www.facebook.com/tr?id=1", L"https://www.youtube.com/api/stats/ads?x", L"https://t.co/i/adsct",
        L"https://cdn.example.org/ads.js", L"https://x.y.z.taboola.com/a", L"https://51.la/", L"https://js.users.51.la/1.js",
    };
    for (const auto* uri : samples) {
        CHECK(Blocks(uri, L"www.example.com", Resource::Other));
        CHECK(Routed(uri));
    }
    CHECK(!Routed(L"https://www.example.com/index.html"));
}

static void TestCosmeticConfig() {
    const std::wstring off = CosmeticScript(false, {});
    CHECK(off.find(L"var CFG={\"enabled\":false,\"off\":[]};") != std::wstring::npos);
    const std::wstring on = CosmeticScript(true, {L"News.Example.com", L"a\"b<c"});
    CHECK(on.find(L"\"off\":[\"news.example.com\",\"a\\\"b\\u003cc\"]") != std::wstring::npos);
    CHECK(on.find(L"__ULB_CONFIG__") == std::wstring::npos);
}

int main() {
    TestHosts();
    TestDomainRules();
    TestFirstPartyExemptions();
    TestPathRules();
    TestSurrogates();
    TestFilterPatterns();
    TestCosmeticConfig();
    if (g_failures) {
        std::fprintf(stderr, "%d adblock check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("adblock rules: all checks passed");
    return EXIT_SUCCESS;
}
