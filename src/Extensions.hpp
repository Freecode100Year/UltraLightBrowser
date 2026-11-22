#pragma once

// Chrome / Edge extensions ("扩展程序"), loaded with WebView2's browser
// extension support. The browser profile is deleted every time the browser
// closes, so installed extensions are kept as copies in %APPDATA%\...\extensions
// (index.json lists them) and added to the profile again at every start.
// Extensions run in normal windows only, not in private windows; their own
// settings and data are cleared on close like other website data.

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <wil/com.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace UltraLight {

class Extensions {
public:
    struct Entry {
        std::string dir;  // folder name under extensions\, the stable key
        std::string name;
        std::string version;
        std::string description;
        std::string popup;
        std::string options;
        bool enabled = true;
        bool proxy = false;  // can take over the browser's proxy
        std::string source;  // key in RecommendedExtensions() when installed from that list
        std::string id;     // WebView2's extension id once loaded this session
        std::string error;  // why loading failed, if it did
    };

    static Extensions& Instance();
    static std::filesystem::path Folder();

    std::vector<Entry> List() const;

    // Copies the extension in and loads it at once (when the profile is known).
    // Returns its name; throws std::runtime_error with a Chinese message.
    std::string InstallFolder(const std::filesystem::path& source);
    std::string InstallPackage(const std::filesystem::path& crxOrZip, const std::string& source = {});
    bool SetEnabled(const std::string& dir, bool enabled);
    bool Remove(const std::string& dir);

    // chrome-extension://<id>/<page>, empty if the extension is not loaded or has no such page.
    std::wstring PageUrl(const std::string& dir, bool options) const;

    // Called with the first normal (not private) WebView: adds every enabled
    // extension to its profile. Later calls do nothing.
    void LoadAll(ICoreWebView2* webView);

private:
    Extensions() = default;
    void LoadIndex();
    bool SaveIndex() const;
    std::string Install(const std::filesystem::path& staged, const std::string& source = {});  // staged folder is moved in
    void AddToProfile(const std::string& dir);

    bool m_indexLoaded = false;
    std::vector<Entry> m_entries;
    wil::com_ptr<ICoreWebView2Profile7> m_profile;
    std::map<std::string, wil::com_ptr<ICoreWebView2BrowserExtension>> m_loaded;  // by dir
};

} // namespace UltraLight
