#pragma once

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include "StringUtils.hpp"

namespace UltraLight {

struct ExtensionInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::filesystem::path unpackedPath;
    std::string defaultPopup;
    std::string defaultIcon;
    bool isEnabled = true;
    wil::com_ptr<ICoreWebView2BrowserExtension> extensionObj;
};

class ExtensionManager {
public:
    static ExtensionManager& Instance();

    // Initialize extension manager with CoreWebView2Environment & CoreWebView2Profile
    void Initialize(ICoreWebView2Environment* env, ICoreWebView2Profile* profile);

    // Interactive load unpacked extension (shows Windows folder picker dialog)
    void PromptLoadUnpackedExtension(HWND hWndParent);

    // Interactive install .CRX extension (shows Windows file picker dialog)
    void PromptInstallCrx(HWND hWndParent);

    // Load an unpacked extension directory into profile with completion callback
    bool LoadUnpackedExtension(
        const std::filesystem::path& extDir,
        std::function<void(bool success, const std::wstring& message)> callback = nullptr
    );

    // Install an extension from a .crx file with completion callback
    bool InstallCrx(
        const std::filesystem::path& crxPath,
        std::function<void(bool success, const std::wstring& message)> callback = nullptr
    );

    // Enable or disable an extension
    void SetExtensionEnabled(const std::string& extId, bool enable, std::function<void(bool success)> callback = nullptr);

    // Remove an extension from the profile
    void RemoveExtension(const std::string& extId, std::function<void(bool success)> callback = nullptr);

    // Reload an extension from its source folder
    void ReloadExtension(const std::string& extId, std::function<void(bool success)> callback = nullptr);

    // Scan and load all installed/persisted extensions
    void LoadInstalledExtensions();

    // Open extensions directory in Windows File Explorer
    void OpenExtensionsDirectory();

    // Spawn extension popup window
    void ShowExtensionPopup(HWND hParent, const ExtensionInfo& ext, POINT anchorPoint);

    // Show Extensions Management Center Dialog
    void ShowExtensionsDialog(HWND hParent);

    const std::vector<ExtensionInfo>& GetExtensions() const { return m_extensions; }

    const ExtensionInfo* FindExtension(const std::string& id) const;

    void SetOnExtensionsChanged(std::function<void()> cb) { m_onExtensionsChanged = cb; }

    ICoreWebView2Environment* GetEnvironment() const { return m_environment.get(); }
    ICoreWebView2Profile* GetProfile() const { return m_profile.get(); }
    ICoreWebView2Profile7* GetProfile7() const { return m_profile7.get(); }

private:
    ExtensionManager() = default;
    ~ExtensionManager() = default;

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    // CRX3 format unpacker
    bool UnpackCrx3(const std::filesystem::path& crxPath, const std::filesystem::path& destDir);

    // Parse manifest.json for extension details
    bool ParseManifest(const std::filesystem::path& extDir, ExtensionInfo& outInfo);

    wil::com_ptr<ICoreWebView2Environment> m_environment;
    wil::com_ptr<ICoreWebView2Profile> m_profile;
    wil::com_ptr<ICoreWebView2Profile7> m_profile7;
    std::vector<ExtensionInfo> m_extensions;
    std::function<void()> m_onExtensionsChanged;
};

} // namespace UltraLight
