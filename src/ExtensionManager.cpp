#include "ExtensionManager.hpp"
#include "Config.hpp"
#include <shlwapi.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <cctype>

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

// Check if entry path contains any path traversal or unsafe characters
static bool IsSafeZipEntryPath(const std::string& filename) {
    if (filename.empty()) return false;
    if (filename.find('\0') != std::string::npos) return false;
    if (filename.find(':') != std::string::npos) return false;
    if (filename.front() == '/' || filename.front() == '\\') return false;

    std::string comp;
    for (size_t i = 0; i <= filename.size(); ++i) {
        char ch = (i == filename.size()) ? '/' : filename[i];
        if (ch == '/' || ch == '\\') {
            if (!comp.empty()) {
                if (comp == ".") {
                    // Current directory segment is allowed
                } else if (comp == "..") {
                    // Path traversal attempt!
                    return false;
                } else {
                    // Check for Windows reserved device names
                    std::string upper = comp;
                    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                    size_t dot = upper.find('.');
                    std::string base = (dot == std::string::npos) ? upper : upper.substr(0, dot);
                    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                        (base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
                         base[3] >= '1' && base[3] <= '9')) {
                        return false;
                    }
                }
                comp.clear();
            }
        } else {
            comp.push_back(ch);
        }
    }
    return true;
}

// Validates CRX3 Protobuf header structure to ensure required signature proofs exist
static bool ValidateCrx3ProtobufHeader(const std::vector<uint8_t>& headerBytes) {
    if (headerBytes.empty()) return false;

    bool hasSignature = false;
    bool hasSignedHeaderData = false;
    size_t offset = 0;

    while (offset < headerBytes.size()) {
        uint64_t tag = 0;
        int shift = 0;
        bool tagOk = false;
        while (offset < headerBytes.size()) {
            uint8_t b = headerBytes[offset++];
            tag |= static_cast<uint64_t>(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) {
                tagOk = true;
                break;
            }
            if (shift >= 64) return false;
        }
        if (!tagOk) return false;

        uint32_t fieldNum = static_cast<uint32_t>(tag >> 3);
        uint32_t wireType = static_cast<uint32_t>(tag & 0x7);

        if (wireType == 0) { // Varint
            while (offset < headerBytes.size() && (headerBytes[offset++] & 0x80) != 0);
        } else if (wireType == 1) { // 64-bit
            if (offset + 8 > headerBytes.size()) return false;
            offset += 8;
        } else if (wireType == 2) { // Length-delimited
            uint64_t len = 0;
            shift = 0;
            bool lenOk = false;
            while (offset < headerBytes.size()) {
                uint8_t b = headerBytes[offset++];
                len |= static_cast<uint64_t>(b & 0x7F) << shift;
                shift += 7;
                if ((b & 0x80) == 0) {
                    lenOk = true;
                    break;
                }
                if (shift >= 64) return false;
            }
            if (!lenOk || offset + len > headerBytes.size()) return false;

            // Field 2: sha256_with_rsa; Field 3: sha256_with_ecdsa
            if ((fieldNum == 2 || fieldNum == 3) && len > 0) {
                hasSignature = true;
            } else if (fieldNum == 10000 && len > 0) {
                // Field 10000: signed_header_data
                hasSignedHeaderData = true;
            }
            offset += static_cast<size_t>(len);
        } else if (wireType == 5) { // 32-bit
            if (offset + 4 > headerBytes.size()) return false;
            offset += 4;
        } else {
            return false; // Unknown or invalid wire type
        }
    }

    return hasSignature && hasSignedHeaderData;
}

// Scans ZIP Central Directory to verify 100% of paths BEFORE extracting to disk
static bool ValidateZipPayloadPreExtraction(std::ifstream& file, uint64_t zipStartOffset, uint64_t fileSize) {
    if (fileSize < zipStartOffset + 22) return false;

    file.seekg(static_cast<std::streamoff>(zipStartOffset), std::ios::beg);
    char zipMagic[4];
    file.read(zipMagic, 4);
    if (!file || zipMagic[0] != 0x50 || zipMagic[1] != 0x4B || zipMagic[2] != 0x03 || zipMagic[3] != 0x04) {
        return false;
    }

    uint64_t maxScan = 65535 + 22;
    uint64_t zipPayloadSize = fileSize - zipStartOffset;
    uint64_t scanSize = (zipPayloadSize < maxScan) ? zipPayloadSize : maxScan;

    std::vector<uint8_t> scanBuf(static_cast<size_t>(scanSize));
    file.seekg(static_cast<std::streamoff>(fileSize - scanSize), std::ios::beg);
    file.read(reinterpret_cast<char*>(scanBuf.data()), static_cast<std::streamsize>(scanSize));
    if (!file) return false;

    int64_t eocdOffsetInBuf = -1;
    for (int64_t i = static_cast<int64_t>(scanSize) - 22; i >= 0; --i) {
        if (scanBuf[i] == 0x50 && scanBuf[i + 1] == 0x4B && scanBuf[i + 2] == 0x05 && scanBuf[i + 3] == 0x06) {
            eocdOffsetInBuf = i;
            break;
        }
    }

    if (eocdOffsetInBuf < 0) return false;

    const uint8_t* eocd = scanBuf.data() + eocdOffsetInBuf;
    uint16_t totalEntries = *reinterpret_cast<const uint16_t*>(eocd + 10);
    uint32_t cdSize = *reinterpret_cast<const uint32_t*>(eocd + 12);
    uint32_t cdOffset = *reinterpret_cast<const uint32_t*>(eocd + 16);

    if (static_cast<uint64_t>(cdOffset) + cdSize > zipPayloadSize) {
        return false;
    }

    uint64_t cdAbsOffset = zipStartOffset + cdOffset;
    file.seekg(static_cast<std::streamoff>(cdAbsOffset), std::ios::beg);

    for (uint16_t entry = 0; entry < totalEntries; ++entry) {
        char cdHeader[46];
        file.read(cdHeader, 46);
        if (!file) return false;

        if (cdHeader[0] != 0x50 || cdHeader[1] != 0x4B || cdHeader[2] != 0x01 || cdHeader[3] != 0x02) {
            return false;
        }

        uint16_t fnLen = *reinterpret_cast<const uint16_t*>(cdHeader + 28);
        uint16_t extraLen = *reinterpret_cast<const uint16_t*>(cdHeader + 30);
        uint16_t commentLen = *reinterpret_cast<const uint16_t*>(cdHeader + 32);

        if (fnLen == 0 || fnLen > 4096) return false;

        std::string filename(fnLen, '\0');
        file.read(&filename[0], fnLen);
        if (!file) return false;

        if (!IsSafeZipEntryPath(filename)) {
            return false;
        }

        if (extraLen + commentLen > 0) {
            file.seekg(extraLen + commentLen, std::ios::cur);
            if (!file) return false;
        }
    }

    return true;
}

bool ExtensionManager::UnpackCrx3(const std::filesystem::path& crxPath, const std::filesystem::path& destDir) {
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(crxPath, ec);
    if (ec || fileSize < 16) return false;

    std::ifstream file(crxPath, std::ios::binary);
    if (!file.is_open()) return false;

    // 1. Read and verify CRX magic
    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "Cr24", 4) != 0) {
        return false; // Not a valid CRX file
    }

    // 2. Verify CRX version
    uint32_t version = 0;
    file.read(reinterpret_cast<char*>(&version), 4);
    if (version != 3) {
        return false; // Not CRX3
    }

    // 3. Read and strictly bounds-check headerSize
    uint32_t headerSize = 0;
    file.read(reinterpret_cast<char*>(&headerSize), 4);
    constexpr uint32_t MAX_CRX_HEADER_SIZE = 16 * 1024 * 1024; // 16 MB max header sanity limit
    if (headerSize == 0 || headerSize > MAX_CRX_HEADER_SIZE || (12ULL + headerSize + 22ULL) > fileSize) {
        return false; // Invalid, truncated, or excessively large header size
    }

    // 4. Verify CRX3 Protobuf signature and signed header data
    std::vector<uint8_t> headerBytes(headerSize);
    file.read(reinterpret_cast<char*>(headerBytes.data()), headerSize);
    if (!file || file.gcount() != static_cast<std::streamsize>(headerSize)) {
        return false;
    }
    if (!ValidateCrx3ProtobufHeader(headerBytes)) {
        return false; // Cryptographic signatures or signed header data missing
    }

    // 5. Pre-extraction ZIP traversal verification: inspect archive headers BEFORE writing to disk
    uint64_t zipStartOffset = 12ULL + headerSize;
    if (!ValidateZipPayloadPreExtraction(file, zipStartOffset, fileSize)) {
        return false; // ZIP contains directory traversal or malformed entries
    }

    // 6. Safe extraction: rewind to ZIP payload start and write temp archive
    file.seekg(static_cast<std::streamoff>(zipStartOffset), std::ios::beg);
    std::filesystem::create_directories(destDir, ec);
    std::filesystem::path tempZip = destDir / "payload.zip";
    {
        std::ofstream zipOut(tempZip, std::ios::binary);
        zipOut << file.rdbuf();
    }

    // 7. Locate system tar.exe via absolute System32 path
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::filesystem::path tarPath = std::filesystem::path(sysDir) / L"tar.exe";
    if (!std::filesystem::exists(tarPath)) {
        std::filesystem::remove(tempZip, ec);
        return false;
    }

    // 8. Direct process invocation avoiding cmd.exe shell injection
    std::wstring cmdLine = L"\"" + tarPath.wstring() + L"\" -xf \"" + tempZip.wstring() + L"\" -C \"" + destDir.wstring() + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    BOOL created = CreateProcessW(
        tarPath.c_str(),
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (created) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    std::filesystem::remove(tempZip, ec);

    // 9. Post-extraction defense-in-depth: second layer canonical verification
    auto canonicalDest = std::filesystem::weakly_canonical(destDir, ec);
    if (!ec) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(destDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            auto canonicalEntry = std::filesystem::weakly_canonical(entry.path(), ec);
            auto rel = std::filesystem::relative(canonicalEntry, canonicalDest, ec);
            if (rel.empty() || rel.string().find("..") != std::string::npos) {
                std::filesystem::remove_all(destDir, ec);
                return false;
            }
        }
    }

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
        std::wstring nameW = L"Extension: " + std::wstring(ext.name.begin(), ext.name.end());
        CreateWindowExW(0, L"STATIC", nameW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        std::wstring popupW = L"Popup Page: " + std::wstring(ext.defaultPopup.begin(), ext.defaultPopup.end());
        CreateWindowExW(0, L"STATIC", popupW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 50, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        std::wstring pathW = L"Path: " + ext.unpackedPath.wstring();
        CreateWindowExW(0, L"STATIC", pathW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 80, width - 40, 60, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        SetFocus(hPopup);
    }
}

} // namespace UltraLight
