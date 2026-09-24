#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <filesystem>

namespace UltraLight {

struct ExtensionInfo {
    std::string id;
    std::string name;
    std::filesystem::path unpackedPath;
    std::string defaultPopup;
    std::string defaultIcon;
    wil::com_ptr<ICoreWebView2BrowserExtension> extensionObj;
};

class ExtensionManager {
public:
    static ExtensionManager& Instance();

    // Initialize extension manager with CoreWebView2Profile
    void Initialize(ICoreWebView2Profile* profile);

    // Install an extension from a .crx file
    bool InstallCrx(const std::filesystem::path& crxPath);

    // Load an unpacked extension directory into profile
    bool LoadUnpackedExtension(const std::filesystem::path& extDir);

    // Scan and load all installed extensions in %LOCALAPPDATA%\UltraLightBrowser\Extensions
    void LoadInstalledExtensions();

    // Spawn extension popup window
    void ShowExtensionPopup(HWND hParent, const ExtensionInfo& ext, POINT anchorPoint);

    const std::vector<ExtensionInfo>& GetExtensions() const { return m_extensions; }

private:
    ExtensionManager() = default;
    ~ExtensionManager() = default;

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    // CRX3 format unpacker
    bool UnpackCrx3(const std::filesystem::path& crxPath, const std::filesystem::path& destDir);

    // Unzip ZIP payload to destination directory
    bool ExtractZipStream(const uint8_t* zipData, size_t zipSize, const std::filesystem::path& destDir);

    // Parse manifest.json for extension details
    bool ParseManifest(const std::filesystem::path& extDir, ExtensionInfo& outInfo);

    wil::com_ptr<ICoreWebView2Profile> m_profile;
    wil::com_ptr<ICoreWebView2Profile7> m_profile7;
    std::vector<ExtensionInfo> m_extensions;
};

} // namespace UltraLight
