#pragma once

#include <windows.h>
#include <commctrl.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <filesystem>
#include "Config.hpp"

namespace UltraLight {

struct ExtensionInfo {
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring description;
    std::wstring folderPath;
    std::wstring optionsPage;
    bool isEnabled = true;
    wil::com_ptr<ICoreWebView2BrowserExtension> comExtension;
};

class ExtensionManager {
public:
    static ExtensionManager& Instance();

    // Lifecycle & Initialization
    void Initialize(ICoreWebView2* webView, ICoreWebView2Environment* env);
    bool IsSupported() const { return m_profile7 != nullptr; }

    // Extension Loading & Actions
    void LoadUnpackedExtension(HWND hWndParent, std::function<void(bool success, const std::wstring& message)> callback = nullptr);
    void AddExtensionFromPath(const std::wstring& folderPath, std::function<void(bool success, const std::wstring& message)> callback = nullptr);
    void RefreshExtensions(std::function<void(bool success)> onComplete = nullptr);
    void ToggleExtension(const std::wstring& id, std::function<void(bool success)> callback = nullptr);
    void SetExtensionEnabled(const std::wstring& id, bool enable, std::function<void(bool success)> callback = nullptr);
    void RemoveExtension(const std::wstring& id, std::function<void(bool success)> callback = nullptr);
    void ReloadAllExtensions();
    void ReloadExtension(const std::wstring& id, std::function<void(bool success)> callback = nullptr);
    void OpenExtensionOptions(const std::wstring& id, ICoreWebView2* webView);
    void OpenExtensionFolder(const std::wstring& id);

    // Queries
    std::vector<ExtensionInfo> GetExtensions() const;
    bool HasExtension(const std::wstring& id) const;
    const ExtensionInfo* FindExtension(const std::wstring& id) const;
    size_t GetExtensionCount() const;

    // UI Dialog
    void ShowManageDialog(HWND hWndParent);

    // Manifest parser
    static bool ParseManifest(const std::filesystem::path& folderPath, ExtensionInfo& outInfo, std::string& outError);

private:
    ExtensionManager() = default;
    ~ExtensionManager() = default;

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    void RestoreSavedExtensions();

    wil::com_ptr<ICoreWebView2> m_webView;
    wil::com_ptr<ICoreWebView2Environment> m_environment;
    wil::com_ptr<ICoreWebView2Profile7> m_profile7;

    std::vector<ExtensionInfo> m_extensions;
    mutable std::mutex m_mutex;
};

} // namespace UltraLight
