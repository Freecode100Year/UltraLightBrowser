#include "ExtensionManager.hpp"
#include "Config.hpp"
#include <shlwapi.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif

namespace UltraLight {

ExtensionManager& ExtensionManager::Instance() {
    static ExtensionManager s_instance;
    return s_instance;
}

void ExtensionManager::Initialize(ICoreWebView2Profile* profile) {
    if (!profile) return;
    m_profile = profile;

    // Query ICoreWebView2Profile7 for Extension APIs (WebView2 SDK v1.0.2210.55+)
    HRESULT hr = m_profile->QueryInterface(IID_PPV_ARGS(&m_profile7));
    if (SUCCEEDED(hr) && m_profile7) {
        LoadInstalledExtensions();
    }
}

bool ExtensionManager::UnpackCrx3(const std::filesystem::path& crxPath, const std::filesystem::path& destDir) {
    std::ifstream file(crxPath, std::ios::binary);
    if (!file.is_open()) return false;

    // Read CRX header
    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "Cr24", 4) != 0) {
        return false; // Not a valid CRX file
    }

    uint32_t version = 0;
    file.read(reinterpret_cast<char*>(&version), 4);
    if (version != 3) {
        return false; // Not CRX3
    }

    uint32_t headerSize = 0;
    file.read(reinterpret_cast<char*>(&headerSize), 4);

    // Skip CRX3 header protobuf bytes to reach ZIP payload
    file.seekg(12 + headerSize, std::ios::beg);

    char zipMagic[4];
    file.read(zipMagic, 4);
    if (zipMagic[0] != 0x50 || zipMagic[1] != 0x4B || zipMagic[2] != 0x03 || zipMagic[3] != 0x04) {
        return false; // Payload is not standard ZIP
    }

    // Rewind 4 bytes to start of ZIP
    file.seekg(12 + headerSize, std::ios::beg);

    std::filesystem::create_directories(destDir);
    std::filesystem::path tempZip = destDir / "payload.zip";
    {
        std::ofstream zipOut(tempZip, std::ios::binary);
        zipOut << file.rdbuf();
    }

    // Unpack ZIP payload using tar -xf (native on Windows 10/11)
    std::wstring cmd = L"tar.exe -xf \"" + tempZip.wstring() + L"\" -C \"" + destDir.wstring() + L"\"";
    _wsystem(cmd.c_str());

    std::filesystem::remove(tempZip);
    return true;
}

bool ExtensionManager::ParseManifest(const std::filesystem::path& extDir, ExtensionInfo& outInfo) {
    std::filesystem::path manifestPath = extDir / "manifest.json";
    if (!std::filesystem::exists(manifestPath)) return false;

#if __has_include(<nlohmann/json.hpp>)
    try {
        std::ifstream file(manifestPath);
        json m;
        file >> m;

        if (m.contains("name")) outInfo.name = m["name"].get<std::string>();

        // Manifest V3 Action
        if (m.contains("action")) {
            auto& action = m["action"];
            if (action.contains("default_popup")) {
                outInfo.defaultPopup = action["default_popup"].get<std::string>();
            }
            if (action.contains("default_icon")) {
                if (action["default_icon"].is_string()) {
                    outInfo.defaultIcon = action["default_icon"].get<std::string>();
                } else if (action["default_icon"].is_object()) {
                    for (auto& [size, iconFile] : action["default_icon"].items()) {
                        outInfo.defaultIcon = iconFile.get<std::string>();
                    }
                }
            }
        }
        // Manifest V2 Browser Action fallback
        else if (m.contains("browser_action")) {
            auto& ba = m["browser_action"];
            if (ba.contains("default_popup")) {
                outInfo.defaultPopup = ba["default_popup"].get<std::string>();
            }
        }
        return true;
    } catch (...) {
        return false;
    }
#else
    outInfo.name = extDir.filename().string();
    return true;
#endif
}

bool ExtensionManager::InstallCrx(const std::filesystem::path& crxPath) {
    std::string extId = crxPath.stem().string();
    std::filesystem::path destDir = Config::Instance().GetExtensionsDirectory() / extId;

    if (!UnpackCrx3(crxPath, destDir)) {
        return false;
    }

    return LoadUnpackedExtension(destDir);
}

bool ExtensionManager::LoadUnpackedExtension(const std::filesystem::path& extDir) {
    if (!m_profile7) return false;

    ExtensionInfo info{};
    info.id = extDir.filename().string();
    info.unpackedPath = extDir;

    ParseManifest(extDir, info);

    HRESULT hr = m_profile7->AddBrowserExtension(
        extDir.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [this, info](HRESULT errorCode, ICoreWebView2BrowserExtension* extension) mutable -> HRESULT {
                if (SUCCEEDED(errorCode) && extension) {
                    info.extensionObj = extension;
                    m_extensions.push_back(info);
                }
                return S_OK;
            }
        ).Get()
    );

    return SUCCEEDED(hr);
}

void ExtensionManager::LoadInstalledExtensions() {
    std::filesystem::path extRoot = Config::Instance().GetExtensionsDirectory();
    if (!std::filesystem::exists(extRoot)) return;

    for (const auto& entry : std::filesystem::directory_iterator(extRoot)) {
        if (entry.is_directory()) {
            LoadUnpackedExtension(entry.path());
        }
    }
}

// Window procedure for extension popup
static LRESULT CALLBACK PopupWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ExtensionManager::ShowExtensionPopup(HWND hParent, const ExtensionInfo& ext, POINT anchorPoint) {
    if (ext.defaultPopup.empty()) return;

    const wchar_t POPUP_CLASS_NAME[] = L"UltraLightExtensionPopupClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = POPUP_CLASS_NAME;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const int width = 380;
    const int height = 480;

    HWND hPopup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        POPUP_CLASS_NAME,
        L"Extension Popup",
        WS_POPUP | WS_BORDER | WS_VISIBLE,
        anchorPoint.x - width + 30,
        anchorPoint.y,
        width,
        height,
        hParent,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (hPopup) {
        SetFocus(hPopup);
    }
}

} // namespace UltraLight
