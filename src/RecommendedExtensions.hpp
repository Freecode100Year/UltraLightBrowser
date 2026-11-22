#pragma once

// The settings page's "推荐扩展" list. Nothing here is shipped inside ULB: each
// extension is downloaded from its official source when the user asks for it,
// and the helper checks it is the genuine file before it is installed
// (a Chrome Web Store CRX by its signature and id, a release ZIP by SHA-256).
// No Windows calls, so it can be unit-tested.
//
// Only extensions that WebView2 actually loads are listed (checked by
// tests/smoke-ui.ps1 on Windows). Violentmonkey 2.50 (MV3) was tried and
// WebView2 refuses it (0x80004005); ULB's own user scripts replace it.

#include <string>
#include <vector>

namespace UltraLight {

struct RecommendedExtension {
    const char* key;          // stable id used by the settings page
    const char* name;
    const char* summary;      // what it is for
    const char* note;         // what to know before installing ("" if nothing)
    const char* homepage;     // official page, opened by "了解更多"
    const char* storeId;      // Chrome Web Store id, or ""
    const char* url;          // release ZIP when there is no store id
    const char* sha256;       // of that ZIP
    bool proxy;               // it takes over the browser's proxy
};

// Chrome Web Store downloads come from Google's update service.
inline std::string StoreDownloadUrl(const std::string& id) {
    return "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=142.0&acceptformat=crx3&x=id%3D" + id + "%26uc";
}

inline const std::vector<RecommendedExtension>& RecommendedExtensions() {
    static const std::vector<RecommendedExtension> list = {
        {"global-speed", "Global Speed：视频速度控制",
         "调整所有网站视频和音频的播放速度，可用快捷键加速、减速、跳过。", "",
         "https://chromewebstore.google.com/detail/jpbjcnkcffbooppibceonlgknpkniiff",
         "jpbjcnkcffbooppibceonlgknpkniiff", "", "", false},
        {"proton-vpn", "Proton VPN",
         "瑞士 Proton 公司的开源（GPL-3.0）VPN 扩展，有免费线路，需要 Proton 账号。",
         "它会接管浏览器的网络代理：开着它时，ULB 自己的 WARP / 我的线路不再生效，「线路不通时不直连」的保护也由它决定。不用时请在这里关掉。",
         "https://protonvpn.com/download-browser-extension",
         "jplgfhpmjnbigmhklmmbgecoobifkmpa", "", "", true},
    };
    return list;
}

inline const RecommendedExtension* FindRecommendedExtension(const std::string& key) {
    for (const auto& e : RecommendedExtensions()) {
        if (key == e.key) return &e;
    }
    return nullptr;
}

} // namespace UltraLight
