#include "ExtensionManager.hpp"
#include "Config.hpp"
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <wincrypt.h>
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

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace UltraLight {

ExtensionManager& ExtensionManager::Instance() {
    static ExtensionManager s_instance;
    return s_instance;
}

void ExtensionManager::Initialize(ICoreWebView2Environment* env, ICoreWebView2Profile* profile) {
    if (!profile) return;
    m_environment = env;
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

// Validates CRX3 Protobuf header structure and extracts the first available public key.
// Note: This validates CRX3 structure and signature presence, but does not verify official Chrome Web Store root CA certificate chains.
static bool ValidateCrx3ProtobufHeader(const std::vector<uint8_t>& headerBytes, std::vector<uint8_t>* outPublicKey = nullptr) {
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
                if (outPublicKey && outPublicKey->empty()) {
                    // AsymmetricKeyProof message: field 1 is public_key (bytes)
                    size_t subOffset = offset;
                    size_t subEnd = offset + static_cast<size_t>(len);
                    while (subOffset < subEnd) {
                        uint64_t subTag = 0;
                        int subShift = 0;
                        bool subTagOk = false;
                        while (subOffset < subEnd) {
                            uint8_t sb = headerBytes[subOffset++];
                            subTag |= static_cast<uint64_t>(sb & 0x7F) << subShift;
                            subShift += 7;
                            if ((sb & 0x80) == 0) { subTagOk = true; break; }
                            if (subShift >= 64) break;
                        }
                        if (!subTagOk) break;

                        uint32_t subField = static_cast<uint32_t>(subTag >> 3);
                        uint32_t subWire = static_cast<uint32_t>(subTag & 0x7);
                        if (subWire == 2) {
                            uint64_t subLen = 0;
                            int slShift = 0;
                            bool slOk = false;
                            while (subOffset < subEnd) {
                                uint8_t lb = headerBytes[subOffset++];
                                subLen |= static_cast<uint64_t>(lb & 0x7F) << slShift;
                                slShift += 7;
                                if ((lb & 0x80) == 0) { slOk = true; break; }
                                if (slShift >= 64) break;
                            }
                            if (!slOk || subOffset + subLen > subEnd) break;
                            if (subField == 1 && subLen > 0) {
                                outPublicKey->assign(
                                    headerBytes.begin() + subOffset,
                                    headerBytes.begin() + subOffset + static_cast<size_t>(subLen)
                                );
                                break;
                            }
                            subOffset += static_cast<size_t>(subLen);
                        } else if (subWire == 0) {
                            while (subOffset < subEnd && (headerBytes[subOffset++] & 0x80) != 0);
                        } else if (subWire == 1) {
                            subOffset += 8;
                        } else if (subWire == 5) {
                            subOffset += 4;
                        } else {
                            break;
                        }
                    }
                }
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

// Derives standard Chrome extension ID (first 16 bytes of SHA-256 of public key, mapped to 'a'-'p')
static std::string CalculateChromeExtensionIdFromPublicKey(const std::vector<uint8_t>& pubKey) {
    if (pubKey.empty()) return "";

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    HCRYPTHASH hHash = 0;
    std::string extId;
    if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        if (CryptHashData(hHash, pubKey.data(), static_cast<DWORD>(pubKey.size()), 0)) {
            uint8_t hash[32]{};
            DWORD hashLen = sizeof(hash);
            if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0) && hashLen >= 16) {
                extId.reserve(32);
                for (size_t i = 0; i < 16; ++i) {
                    extId += static_cast<char>('a' + ((hash[i] >> 4) & 0x0F));
                    extId += static_cast<char>('a' + (hash[i] & 0x0F));
                }
            }
        }
        CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
    return extId;
}

static std::string ExtractCrx3ExtensionId(const std::filesystem::path& crxPath) {
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(crxPath, ec);
    if (ec || fileSize < 16) return "";

    std::ifstream file(crxPath, std::ios::binary);
    if (!file.is_open()) return "";

    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, "Cr24", 4) != 0) return "";

    uint32_t version = 0;
    file.read(reinterpret_cast<char*>(&version), 4);
    if (version != 3) return "";

    uint32_t headerSize = 0;
    file.read(reinterpret_cast<char*>(&headerSize), 4);
    if (headerSize == 0 || headerSize > 16 * 1024 * 1024 || (12ULL + headerSize + 22ULL) > fileSize) {
        return "";
    }

    std::vector<uint8_t> headerBytes(headerSize);
    file.read(reinterpret_cast<char*>(headerBytes.data()), headerSize);
    if (!file || file.gcount() != static_cast<std::streamsize>(headerSize)) {
        return "";
    }

    std::vector<uint8_t> pubKey;
    if (ValidateCrx3ProtobufHeader(headerBytes, &pubKey)) {
        return CalculateChromeExtensionIdFromPublicKey(pubKey);
    }
    return "";
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

    // 6. Safe extraction: extract into an isolated temporary staging directory
    std::filesystem::path tempBase = std::filesystem::temp_directory_path(ec);
    if (ec) {
        tempBase = destDir.parent_path() / L".staging";
    }
    std::wstring stageFolderName = L"ulb_ext_stage_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::filesystem::path stageDir = tempBase / stageFolderName;
    std::filesystem::create_directories(stageDir, ec);

    // Guaranteed staging directory cleanup on exit
    auto cleanupStaging = wil::scope_exit([&]() {
        std::error_code ignoreEc;
        std::filesystem::remove_all(stageDir, ignoreEc);
    });

    file.seekg(static_cast<std::streamoff>(zipStartOffset), std::ios::beg);
    std::filesystem::path tempZip = stageDir / "payload.zip";
    {
        std::ofstream zipOut(tempZip, std::ios::binary);
        zipOut << file.rdbuf();
    }

    // 7. Locate system tar.exe via absolute System32 path
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::filesystem::path tarPath = std::filesystem::path(sysDir) / L"tar.exe";
    if (!std::filesystem::exists(tarPath)) {
        return false;
    }

    // 8. Direct process invocation avoiding cmd.exe shell injection
    std::wstring cmdLine = L"\"" + tarPath.wstring() + L"\" -xf \"" + tempZip.wstring() + L"\" -C \"" + stageDir.wstring() + L"\"";
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
        DWORD waitRes = WaitForSingleObject(pi.hProcess, 15000);
        DWORD exitCode = 1;
        if (waitRes == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exitCode);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::filesystem::remove(tempZip, ec);
        if (exitCode != 0) {
            return false;
        }
    } else {
        return false;
    }

    // 9. Post-extraction defense-in-depth: canonical directory traversal verification within staging
    auto canonicalStage = std::filesystem::weakly_canonical(stageDir, ec);
    if (!ec) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(stageDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            auto canonicalEntry = std::filesystem::weakly_canonical(entry.path(), ec);
            auto rel = std::filesystem::relative(canonicalEntry, canonicalStage, ec);
            if (rel.empty() || rel.wstring().find(L"..") != std::wstring::npos) {
                return false;
            }
        }
    }

    // 10. Verify manifest.json exists before committing to destDir
    if (!std::filesystem::exists(stageDir / "manifest.json")) {
        return false;
    }

    // 11. Atomic replace into destDir (only touched upon 100% verified extraction)
    std::filesystem::create_directories(destDir.parent_path(), ec);
    if (std::filesystem::exists(destDir)) {
        std::filesystem::remove_all(destDir, ec);
    }

    std::filesystem::rename(stageDir, destDir, ec);
    if (ec) {
        std::filesystem::copy(stageDir, destDir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    }

    return !ec;
}

using UltraLight::StringUtils::Utf8ToWide;
using UltraLight::StringUtils::WideToUtf8;

static std::wstring FormatHResult(HRESULT hr) {
    wchar_t buf[32];
    swprintf_s(buf, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

static std::string ReadLocaleString(const std::filesystem::path& extDir, const std::string& key) {
#if __has_include(<nlohmann/json.hpp>)
    const std::vector<std::string> locales = { "zh_CN", "zh", "zh_TW", "en", "en_US", "en_GB" };
    for (const auto& loc : locales) {
        std::filesystem::path locFile = extDir / "_locales" / loc / "messages.json";
        if (std::filesystem::exists(locFile)) {
            try {
                std::ifstream f(locFile);
                json j;
                f >> j;
                if (j.contains(key) && j[key].contains("message")) {
                    return j[key]["message"].get<std::string>();
                }
            } catch (...) {}
        }
    }
#endif
    return "";
}

bool ExtensionManager::ParseManifest(const std::filesystem::path& extDir, ExtensionInfo& outInfo) {
    std::filesystem::path manifestPath = extDir / "manifest.json";
    if (!std::filesystem::exists(manifestPath)) return false;

#if __has_include(<nlohmann/json.hpp>)
    try {
        std::ifstream file(manifestPath);
        json m;
        file >> m;

        if (m.contains("name") && m["name"].is_string()) {
            std::string rawName = m["name"].get<std::string>();
            if (rawName.rfind("__MSG_", 0) == 0 && rawName.ends_with("__") && rawName.size() > 8) {
                std::string key = rawName.substr(6, rawName.size() - 8);
                std::string localized = ReadLocaleString(extDir, key);
                outInfo.name = localized.empty() ? rawName : localized;
            } else {
                outInfo.name = rawName;
            }
        }

        if (m.contains("version") && m["version"].is_string()) {
            outInfo.version = m["version"].get<std::string>();
        }

        if (m.contains("description") && m["description"].is_string()) {
            std::string rawDesc = m["description"].get<std::string>();
            if (rawDesc.rfind("__MSG_", 0) == 0 && rawDesc.ends_with("__") && rawDesc.size() > 8) {
                std::string key = rawDesc.substr(6, rawDesc.size() - 8);
                std::string localized = ReadLocaleString(extDir, key);
                outInfo.description = localized.empty() ? rawDesc : localized;
            } else {
                outInfo.description = rawDesc;
            }
        }

        // Manifest V3 Action
        if (m.contains("action") && m["action"].is_object()) {
            auto& action = m["action"];
            if (action.contains("default_popup") && action["default_popup"].is_string()) {
                outInfo.defaultPopup = action["default_popup"].get<std::string>();
            }
            if (action.contains("default_icon")) {
                if (action["default_icon"].is_string()) {
                    outInfo.defaultIcon = action["default_icon"].get<std::string>();
                } else if (action["default_icon"].is_object()) {
                    for (auto& [size, iconFile] : action["default_icon"].items()) {
                        if (iconFile.is_string()) {
                            outInfo.defaultIcon = iconFile.get<std::string>();
                        }
                    }
                }
            }
        }
        // Manifest V2 Browser Action fallback
        else if (m.contains("browser_action") && m["browser_action"].is_object()) {
            auto& ba = m["browser_action"];
            if (ba.contains("default_popup") && ba["default_popup"].is_string()) {
                outInfo.defaultPopup = ba["default_popup"].get<std::string>();
            }
            if (ba.contains("default_icon")) {
                if (ba["default_icon"].is_string()) {
                    outInfo.defaultIcon = ba["default_icon"].get<std::string>();
                } else if (ba["default_icon"].is_object()) {
                    for (auto& [size, iconFile] : ba["default_icon"].items()) {
                        if (iconFile.is_string()) {
                            outInfo.defaultIcon = iconFile.get<std::string>();
                        }
                    }
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
#else
    outInfo.name = WideToUtf8(extDir.filename().wstring());
    return true;
#endif
}

const ExtensionInfo* ExtensionManager::FindExtension(const std::string& id) const {
    for (const auto& ext : m_extensions) {
        if (ext.id == id) return &ext;
    }
    return nullptr;
}

bool ExtensionManager::LoadUnpackedExtension(
    const std::filesystem::path& extDir,
    std::function<void(bool success, const std::wstring& message)> callback
) {
    if (!std::filesystem::exists(extDir) || !std::filesystem::is_directory(extDir)) {
        if (callback) callback(false, L"指定的扩展程序目录不存在！");
        return false;
    }

    if (!std::filesystem::exists(extDir / "manifest.json")) {
        if (callback) callback(false, L"目录中未找到 manifest.json 清单文件！");
        return false;
    }

    if (!m_profile7) {
        if (callback) callback(false, L"当前环境不支持 WebView2 扩展程序 (ICoreWebView2Profile7 未就绪)。");
        return false;
    }

    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(extDir, ec);
    if (ec) canonicalPath = extDir;

    ExtensionInfo info{};
    info.unpackedPath = canonicalPath;
    ParseManifest(canonicalPath, info);
    if (info.name.empty()) {
        info.name = WideToUtf8(canonicalPath.filename().wstring());
    }

    HRESULT hr = m_profile7->AddBrowserExtension(
        canonicalPath.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [this, info, callback](HRESULT errorCode, ICoreWebView2BrowserExtension* extension) mutable -> HRESULT {
                if (SUCCEEDED(errorCode) && extension) {
                    wil::unique_cotaskmem_string idStr;
                    if (SUCCEEDED(extension->get_Id(&idStr)) && idStr.get()) {
                        info.id = WideToUtf8(idStr.get());
                    }
                    wil::unique_cotaskmem_string nameStr;
                    if (SUCCEEDED(extension->get_Name(&nameStr)) && nameStr.get() && info.name.empty()) {
                        info.name = WideToUtf8(nameStr.get());
                    }
                    BOOL isEnabled = TRUE;
                    extension->get_IsEnabled(&isEnabled);
                    info.isEnabled = (isEnabled != FALSE);
                    info.extensionObj = extension;

                    // Update existing or add new
                    auto it = std::find_if(m_extensions.begin(), m_extensions.end(), [&](const ExtensionInfo& e) {
                        return (!info.id.empty() && e.id == info.id) ||
                               (!info.unpackedPath.empty() && e.unpackedPath == info.unpackedPath);
                    });
                    if (it != m_extensions.end()) {
                        *it = info;
                    } else {
                        m_extensions.push_back(info);
                    }

                    Config::Instance().AddUnpackedExtensionPath(info.unpackedPath);

                    if (m_onExtensionsChanged) {
                        m_onExtensionsChanged();
                    }

                    if (callback) {
                        std::wstring msg = L"扩展程序加载成功！\n\n名称: " + Utf8ToWide(info.name) +
                                           L"\n版本: " + Utf8ToWide(info.version.empty() ? "1.0" : info.version) +
                                           L"\nID: " + Utf8ToWide(info.id);
                        callback(true, msg);
                    }
                } else {
                    std::wstring errMsg = L"无法加载扩展程序 (错误代码: " + FormatHResult(errorCode) + L")";
                    if (errorCode == 0x80070032) { // ERROR_NOT_SUPPORTED
                        errMsg += L"\n提示: WebView2 扩展功能未开启或当前 Runtime 不支持扩展 API。";
                    } else if (errorCode == E_FAIL) {
                        errMsg += L"\n请检查 manifest.json 文件内容或扩展代码结构。";
                    }
                    if (callback) {
                        callback(false, errMsg);
                    }
                }
                return S_OK;
            }
        ).Get()
    );

    if (FAILED(hr)) {
        if (callback) {
            callback(false, L"调用 AddBrowserExtension 失败: " + FormatHResult(hr));
        }
        return false;
    }

    return true;
}

void ExtensionManager::PromptLoadUnpackedExtension(HWND hWndParent) {
    wil::com_ptr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) {
        MessageBoxW(hWndParent, L"无法创建文件夹选择对话框 (COM 初始化错误)。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    DWORD dwOptions = 0;
    if (SUCCEEDED(fileDialog->GetOptions(&dwOptions))) {
        fileDialog->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    fileDialog->SetTitle(L"选择未打包的扩展程序根目录 (包含 manifest.json)");

    hr = fileDialog->Show(hWndParent);
    if (FAILED(hr)) {
        return;
    }

    wil::com_ptr<IShellItem> item;
    hr = fileDialog->GetResult(&item);
    if (FAILED(hr) || !item) return;

    wil::unique_cotaskmem_string folderPath;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &folderPath);
    if (FAILED(hr) || !folderPath.get()) return;

    std::filesystem::path selectedDir(folderPath.get());

    if (!std::filesystem::exists(selectedDir / "manifest.json")) {
        MessageBoxW(
            hWndParent,
            L"无法加载扩展程序：\n所选目录中未找到 manifest.json 清单文件。\n\n请确保选择的是包含 manifest.json 的扩展程序根目录！",
            L"缺少清单文件",
            MB_OK | MB_ICONWARNING
        );
        return;
    }

    LoadUnpackedExtension(selectedDir, [hWndParent](bool success, const std::wstring& message) {
        MessageBoxW(
            hWndParent,
            message.c_str(),
            success ? L"扩展程序加载成功" : L"加载扩展程序失败",
            MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR)
        );
    });
}

bool ExtensionManager::InstallCrx(
    const std::filesystem::path& crxPath,
    std::function<void(bool success, const std::wstring& message)> callback
) {
    if (!std::filesystem::exists(crxPath)) {
        if (callback) callback(false, L"指定的 CRX 文件不存在！");
        return false;
    }

    // 1. Compute official 32-character Chrome extension ID from CRX3 public key
    std::string extId = ExtractCrx3ExtensionId(crxPath);
    if (extId.empty()) {
        // Fallback: derive safe alphanumeric identifier from filename stem
        std::wstring stemW = crxPath.stem().wstring();
        for (wchar_t ch : stemW) {
            if (iswalnum(ch) || ch == L'_' || ch == L'-') {
                extId.push_back(static_cast<char>(ch));
            }
        }
        if (extId.empty() || extId == "." || extId == "..") {
            extId = "crx_ext_" + std::to_string(GetTickCount64());
        }
    }

    std::filesystem::path extensionsDir = Config::Instance().GetExtensionsDirectory();
    std::filesystem::path destDir = extensionsDir / extId;

    // 2. Strict boundary validation: verify destDir is directly under extensionsDir
    std::error_code ec;
    auto canDest = std::filesystem::weakly_canonical(destDir, ec);
    auto canExtDir = std::filesystem::weakly_canonical(extensionsDir, ec);
    if (canDest.parent_path() != canExtDir) {
        if (callback) callback(false, L"非法的文件名或安装路径！");
        return false;
    }

    if (!UnpackCrx3(crxPath, destDir)) {
        if (callback) callback(false, L"解压 CRX 失败：文件损坏或格式不受支持。");
        return false;
    }

    return LoadUnpackedExtension(destDir, callback);
}

void ExtensionManager::PromptInstallCrx(HWND hWndParent) {
    wil::com_ptr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) {
        MessageBoxW(hWndParent, L"无法创建文件选择对话框 (COM 初始化错误)。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    COMDLG_FILTERSPEC fileTypes[] = {
        { L"Chrome 扩展程序 (*.crx)", L"*.crx" },
        { L"所有文件 (*.*)", L"*.*" }
    };
    fileDialog->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);
    fileDialog->SetTitle(L"选择要安装的 Chrome 扩展程序 (.crx 文件)");

    hr = fileDialog->Show(hWndParent);
    if (FAILED(hr)) return;

    wil::com_ptr<IShellItem> item;
    hr = fileDialog->GetResult(&item);
    if (FAILED(hr) || !item) return;

    wil::unique_cotaskmem_string filePath;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
    if (FAILED(hr) || !filePath.get()) return;

    std::filesystem::path selectedFile(filePath.get());
    InstallCrx(selectedFile, [hWndParent](bool success, const std::wstring& message) {
        MessageBoxW(
            hWndParent,
            message.c_str(),
            success ? L"CRX 安装成功" : L"CRX 安装失败",
            MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR)
        );
    });
}

void ExtensionManager::OpenExtensionsDirectory() {
    auto dir = Config::Instance().GetExtensionsDirectory();
    std::filesystem::create_directories(dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ExtensionManager::SetExtensionEnabled(const std::string& extId, bool enable, std::function<void(bool success)> callback) {
    for (auto& ext : m_extensions) {
        if (ext.id == extId && ext.extensionObj) {
            ext.extensionObj->Enable(
                enable ? TRUE : FALSE,
                Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                    [this, extId, enable, callback](HRESULT errorCode) -> HRESULT {
                        bool ok = SUCCEEDED(errorCode);
                        if (ok) {
                            for (auto& e : m_extensions) {
                                if (e.id == extId) {
                                    e.isEnabled = enable;
                                    break;
                                }
                            }
                            if (m_onExtensionsChanged) m_onExtensionsChanged();
                        }
                        if (callback) callback(ok);
                        return S_OK;
                    }
                ).Get()
            );
            return;
        }
    }
    if (callback) callback(false);
}

void ExtensionManager::RemoveExtension(const std::string& extId, std::function<void(bool success)> callback) {
    for (auto it = m_extensions.begin(); it != m_extensions.end(); ++it) {
        if (it->id == extId) {
            std::filesystem::path unpackedPath = it->unpackedPath;
            if (it->extensionObj) {
                it->extensionObj->Remove(
                    Microsoft::WRL::Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
                        [this, extId, unpackedPath, callback](HRESULT errorCode) -> HRESULT {
                            bool ok = SUCCEEDED(errorCode);
                            if (ok) {
                                auto iter = std::find_if(m_extensions.begin(), m_extensions.end(), [&](const ExtensionInfo& e) {
                                    return e.id == extId;
                                });
                                if (iter != m_extensions.end()) {
                                    m_extensions.erase(iter);
                                }
                                Config::Instance().RemoveUnpackedExtensionPath(unpackedPath);
                                if (m_onExtensionsChanged) m_onExtensionsChanged();
                            }
                            if (callback) callback(ok);
                            return S_OK;
                        }
                    ).Get()
                );
            } else {
                Config::Instance().RemoveUnpackedExtensionPath(unpackedPath);
                m_extensions.erase(it);
                if (m_onExtensionsChanged) m_onExtensionsChanged();
                if (callback) callback(true);
            }
            return;
        }
    }
    if (callback) callback(false);
}

void ExtensionManager::ReloadExtension(const std::string& extId, std::function<void(bool success)> callback) {
    for (const auto& ext : m_extensions) {
        if (ext.id == extId) {
            if (!ext.unpackedPath.empty()) {
                LoadUnpackedExtension(ext.unpackedPath, [callback](bool success, const std::wstring&) {
                    if (callback) callback(success);
                });
            } else {
                if (callback) callback(false);
            }
            return;
        }
    }
    if (callback) callback(false);
}

void ExtensionManager::LoadInstalledExtensions() {
    if (!m_profile7) return;

    // 1. Query WebView2 profile for already-installed extensions
    m_profile7->GetBrowserExtensions(
        Microsoft::WRL::Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [this](HRESULT errorCode, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                if (SUCCEEDED(errorCode) && list) {
                    UINT32 count = 0;
                    list->get_Count(&count);
                    for (UINT32 i = 0; i < count; ++i) {
                        wil::com_ptr<ICoreWebView2BrowserExtension> ext;
                        if (SUCCEEDED(list->GetValueAtIndex(i, &ext)) && ext) {
                            wil::unique_cotaskmem_string idStr;
                            ext->get_Id(&idStr);
                            wil::unique_cotaskmem_string nameStr;
                            ext->get_Name(&nameStr);
                            BOOL isEnabled = TRUE;
                            ext->get_IsEnabled(&isEnabled);

                            std::string id = idStr.get() ? WideToUtf8(idStr.get()) : "";
                            std::string name = nameStr.get() ? WideToUtf8(nameStr.get()) : "";

                            auto it = std::find_if(m_extensions.begin(), m_extensions.end(), [&](const ExtensionInfo& e) {
                                return e.id == id;
                            });
                            if (it != m_extensions.end()) {
                                it->extensionObj = ext;
                                it->isEnabled = (isEnabled != FALSE);
                                if (!name.empty() && it->name.empty()) it->name = name;
                            } else {
                                ExtensionInfo info{};
                                info.id = id;
                                info.name = name;
                                info.isEnabled = (isEnabled != FALSE);
                                info.extensionObj = ext;
                                m_extensions.push_back(info);
                            }
                        }
                    }
                }

                // 2. Load any unpacked extensions persisted in config
                auto savedPaths = Config::Instance().GetUnpackedExtensionPaths();
                for (const auto& path : savedPaths) {
                    if (std::filesystem::exists(path / "manifest.json")) {
                        auto it = std::find_if(m_extensions.begin(), m_extensions.end(), [&](const ExtensionInfo& e) {
                            return e.unpackedPath == path;
                        });
                        if (it == m_extensions.end()) {
                            LoadUnpackedExtension(path, nullptr);
                        }
                    }
                }

                // 3. Scan %LOCALAPPDATA%\UltraLightBrowser\Extensions
                std::filesystem::path extRoot = Config::Instance().GetExtensionsDirectory();
                if (std::filesystem::exists(extRoot)) {
                    for (const auto& entry : std::filesystem::directory_iterator(extRoot)) {
                        if (entry.is_directory() && std::filesystem::exists(entry.path() / "manifest.json")) {
                            auto it = std::find_if(m_extensions.begin(), m_extensions.end(), [&](const ExtensionInfo& e) {
                                return e.unpackedPath == entry.path();
                            });
                            if (it == m_extensions.end()) {
                                LoadUnpackedExtension(entry.path(), nullptr);
                            }
                        }
                    }
                }

                if (m_onExtensionsChanged) m_onExtensionsChanged();
                return S_OK;
            }
        ).Get()
    );
}

struct PopupContext {
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> webView;
};

static LRESULT CALLBACK PopupWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_SIZE: {
        auto* ctx = reinterpret_cast<PopupContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (ctx && ctx->controller) {
            RECT client;
            GetClientRect(hWnd, &client);
            ctx->controller->put_Bounds(client);
        }
        break;
    }
    case WM_DESTROY: {
        auto* ctx = reinterpret_cast<PopupContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (ctx) {
            if (ctx->controller) {
                ctx->controller->Close();
            }
            delete ctx;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ExtensionManager::ShowExtensionPopup(HWND hParent, const ExtensionInfo& ext, POINT anchorPoint) {
    const wchar_t POPUP_CLASS_NAME[] = L"UltraLightExtensionPopupClass";
    static bool s_popupClassRegistered = false;
    if (!s_popupClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = PopupWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = POPUP_CLASS_NAME;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc)) {
            s_popupClassRegistered = true;
        }
    }

    const int width = 400;
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

    if (!hPopup) return;

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hPopup, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hPopup, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    auto* ctx = new PopupContext();
    SetWindowLongPtrW(hPopup, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

    if (!ext.defaultPopup.empty() && m_environment) {
        m_environment->CreateCoreWebView2Controller(
            hPopup,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [hPopup, ext](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                    if (SUCCEEDED(hr) && controller && IsWindow(hPopup)) {
                        auto* pCtx = reinterpret_cast<PopupContext*>(GetWindowLongPtrW(hPopup, GWLP_USERDATA));
                        if (pCtx) {
                            pCtx->controller = controller;
                            controller->get_CoreWebView2(&pCtx->webView);

                            RECT client;
                            GetClientRect(hPopup, &client);
                            controller->put_Bounds(client);
                            controller->put_IsVisible(TRUE);

                            if (pCtx->webView) {
                                std::wstring extUrl = L"chrome-extension://" + Utf8ToWide(ext.id) + L"/" + Utf8ToWide(ext.defaultPopup);
                                pCtx->webView->Navigate(extUrl.c_str());
                            }
                        }
                    }
                    return S_OK;
                }
            ).Get()
        );
    } else {
        std::wstring nameW = L"扩展名称: " + Utf8ToWide(ext.name.empty() ? ext.id : ext.name);
        CreateWindowExW(0, L"STATIC", nameW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        std::wstring verW = L"版本: " + Utf8ToWide(ext.version.empty() ? "1.0" : ext.version);
        CreateWindowExW(0, L"STATIC", verW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 50, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        std::wstring idW = L"ID: " + Utf8ToWide(ext.id);
        CreateWindowExW(0, L"STATIC", idW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 80, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        std::wstring statusW = L"状态: " + std::wstring(ext.isEnabled ? L"已启用" : L"已禁用");
        CreateWindowExW(0, L"STATIC", statusW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 110, width - 40, 25, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        if (!ext.unpackedPath.empty()) {
            std::wstring pathW = L"路径: " + ext.unpackedPath.wstring();
            CreateWindowExW(0, L"STATIC", pathW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 140, width - 40, 50, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        }
        if (!ext.description.empty()) {
            std::wstring descW = L"说明: " + Utf8ToWide(ext.description);
            CreateWindowExW(0, L"STATIC", descW.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 200, width - 40, 60, hPopup, nullptr, GetModuleHandle(nullptr), nullptr);
        }
    }

    SetFocus(hPopup);
}

enum ExtDlgID : WORD {
    IDC_EDLG_LOAD_UNPACKED = 3001,
    IDC_EDLG_INSTALL_CRX   = 3002,
    IDC_EDLG_OPEN_DIR      = 3003,
    IDC_EDLG_REFRESH       = 3004,
    IDC_EDLG_LIST          = 3005,
    IDC_EDLG_TOGGLE        = 3006,
    IDC_EDLG_RELOAD        = 3007,
    IDC_EDLG_POPUP         = 3008,
    IDC_EDLG_REMOVE        = 3009,
    IDC_EDLG_CLOSE         = 3010
};

static HWND s_hExtensionsDlg = nullptr;

struct ExtDlgData {
    HWND hBtnLoad = nullptr;
    HWND hBtnInstall = nullptr;
    HWND hBtnOpenDir = nullptr;
    HWND hBtnRefresh = nullptr;
    HWND hList = nullptr;
    HWND hBtnToggle = nullptr;
    HWND hBtnReload = nullptr;
    HWND hBtnPopup = nullptr;
    HWND hBtnRemove = nullptr;
    HWND hBtnClose = nullptr;
    HFONT hFont = nullptr;
    UINT dpi = 96;
};

static void PopulateExtensionsListView(HWND hList) {
    if (!hList) return;
    ListView_DeleteAllItems(hList);
    const auto& exts = ExtensionManager::Instance().GetExtensions();
    for (size_t i = 0; i < exts.size(); ++i) {
        const auto& ext = exts[i];

        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = static_cast<int>(i);
        lvi.iSubItem = 0;
        std::wstring nameW = Utf8ToWide(ext.name.empty() ? ext.id : ext.name);
        lvi.pszText = const_cast<LPWSTR>(nameW.c_str());
        lvi.lParam = static_cast<LPARAM>(i);
        int itemIndex = ListView_InsertItem(hList, &lvi);

        std::wstring statusW = ext.isEnabled ? L"已启用" : L"已禁用";
        ListView_SetItemText(hList, itemIndex, 1, const_cast<LPWSTR>(statusW.c_str()));

        std::wstring verW = Utf8ToWide(ext.version.empty() ? "1.0" : ext.version);
        ListView_SetItemText(hList, itemIndex, 2, const_cast<LPWSTR>(verW.c_str()));

        std::wstring idW = Utf8ToWide(ext.id);
        ListView_SetItemText(hList, itemIndex, 3, const_cast<LPWSTR>(idW.c_str()));

        std::wstring pathW = ext.unpackedPath.wstring();
        ListView_SetItemText(hList, itemIndex, 4, const_cast<LPWSTR>(pathW.c_str()));
    }
}

static void UpdateExtDlgLayout(HWND hWnd, ExtDlgData* d, int width, int height) {
    if (!d || width <= 0 || height <= 0) return;
    UINT dpi = d->dpi;
    int pad = MulDiv(10, dpi, 96);
    int btnH = MulDiv(30, dpi, 96);

    int topY = pad;
    int btnLoadW = MulDiv(190, dpi, 96);
    int btnInstW = MulDiv(120, dpi, 96);
    int btnDirW = MulDiv(120, dpi, 96);
    int btnRefW = MulDiv(70, dpi, 96);

    SetWindowPos(d->hBtnLoad, nullptr, pad, topY, btnLoadW, btnH, SWP_NOZORDER);
    SetWindowPos(d->hBtnInstall, nullptr, pad + btnLoadW + pad, topY, btnInstW, btnH, SWP_NOZORDER);
    SetWindowPos(d->hBtnOpenDir, nullptr, pad + btnLoadW + pad + btnInstW + pad, topY, btnDirW, btnH, SWP_NOZORDER);
    SetWindowPos(d->hBtnRefresh, nullptr, pad + btnLoadW + pad + btnInstW + pad + btnDirW + pad, topY, btnRefW, btnH, SWP_NOZORDER);

    int bottomY = height - pad - btnH;
    int bTogW = MulDiv(90, dpi, 96);
    int bRelW = MulDiv(90, dpi, 96);
    int bPopW = MulDiv(100, dpi, 96);
    int bRemW = MulDiv(90, dpi, 96);
    int bClsW = MulDiv(80, dpi, 96);

    int bx = pad;
    SetWindowPos(d->hBtnToggle, nullptr, bx, bottomY, bTogW, btnH, SWP_NOZORDER);
    bx += bTogW + pad;
    SetWindowPos(d->hBtnReload, nullptr, bx, bottomY, bRelW, btnH, SWP_NOZORDER);
    bx += bRelW + pad;
    SetWindowPos(d->hBtnPopup, nullptr, bx, bottomY, bPopW, btnH, SWP_NOZORDER);
    bx += bPopW + pad;
    SetWindowPos(d->hBtnRemove, nullptr, bx, bottomY, bRemW, btnH, SWP_NOZORDER);

    SetWindowPos(d->hBtnClose, nullptr, width - pad - bClsW, bottomY, bClsW, btnH, SWP_NOZORDER);

    int listY = topY + btnH + pad;
    int listH = bottomY - pad - listY;
    int listW = width - pad * 2;
    SetWindowPos(d->hList, nullptr, pad, listY, listW, listH, SWP_NOZORDER);
}

static LRESULT CALLBACK ExtensionsDlgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* d = reinterpret_cast<ExtDlgData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        d = new ExtDlgData();
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        d->dpi = GetDpiForWindow(hWnd);

        int fontH = -MulDiv(9, d->dpi, 72);
        d->hFont = CreateFontW(
            fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

        HINSTANCE hInst = GetModuleHandle(nullptr);
        d->hBtnLoad = CreateWindowExW(0, L"BUTTON", L"📂 加载未打包的扩展程序...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_LOAD_UNPACKED), hInst, nullptr);
        d->hBtnInstall = CreateWindowExW(0, L"BUTTON", L"📦 安装 .CRX...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_INSTALL_CRX), hInst, nullptr);
        d->hBtnOpenDir = CreateWindowExW(0, L"BUTTON", L"📁 打开扩展目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_OPEN_DIR), hInst, nullptr);
        d->hBtnRefresh = CreateWindowExW(0, L"BUTTON", L"⟳ 刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_REFRESH), hInst, nullptr);

        d->hList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_LIST), hInst, nullptr
        );
        ListView_SetExtendedListViewStyle(d->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        // Columns
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0; lvc.cx = MulDiv(180, d->dpi, 96); lvc.pszText = const_cast<LPWSTR>(L"扩展名称");
        ListView_InsertColumn(d->hList, 0, &lvc);

        lvc.iSubItem = 1; lvc.cx = MulDiv(70, d->dpi, 96); lvc.pszText = const_cast<LPWSTR>(L"状态");
        ListView_InsertColumn(d->hList, 1, &lvc);

        lvc.iSubItem = 2; lvc.cx = MulDiv(65, d->dpi, 96); lvc.pszText = const_cast<LPWSTR>(L"版本");
        ListView_InsertColumn(d->hList, 2, &lvc);

        lvc.iSubItem = 3; lvc.cx = MulDiv(160, d->dpi, 96); lvc.pszText = const_cast<LPWSTR>(L"扩展 ID");
        ListView_InsertColumn(d->hList, 3, &lvc);

        lvc.iSubItem = 4; lvc.cx = MulDiv(220, d->dpi, 96); lvc.pszText = const_cast<LPWSTR>(L"本地所在路径");
        ListView_InsertColumn(d->hList, 4, &lvc);

        d->hBtnToggle = CreateWindowExW(0, L"BUTTON", L"启用 / 禁用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_TOGGLE), hInst, nullptr);
        d->hBtnReload = CreateWindowExW(0, L"BUTTON", L"⟳ 重新加载", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_RELOAD), hInst, nullptr);
        d->hBtnPopup = CreateWindowExW(0, L"BUTTON", L"打开弹窗", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_POPUP), hInst, nullptr);
        d->hBtnRemove = CreateWindowExW(0, L"BUTTON", L"🗑 移除扩展", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_REMOVE), hInst, nullptr);
        d->hBtnClose = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EDLG_CLOSE), hInst, nullptr);

        // Apply font
        HWND ctrls[] = { d->hBtnLoad, d->hBtnInstall, d->hBtnOpenDir, d->hBtnRefresh, d->hList, d->hBtnToggle, d->hBtnReload, d->hBtnPopup, d->hBtnRemove, d->hBtnClose };
        for (HWND h : ctrls) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d->hFont), TRUE);
        }

        PopulateExtensionsListView(d->hList);

        // Register change listener
        ExtensionManager::Instance().SetOnExtensionsChanged([hWnd]() {
            if (IsWindow(hWnd)) {
                auto* data = reinterpret_cast<ExtDlgData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
                if (data && data->hList) {
                    PopulateExtensionsListView(data->hList);
                }
            }
        });

        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        UpdateExtDlgLayout(hWnd, d, w, h);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDC_EDLG_LOAD_UNPACKED:
            ExtensionManager::Instance().PromptLoadUnpackedExtension(hWnd);
            break;
        case IDC_EDLG_INSTALL_CRX:
            ExtensionManager::Instance().PromptInstallCrx(hWnd);
            break;
        case IDC_EDLG_OPEN_DIR:
            ExtensionManager::Instance().OpenExtensionsDirectory();
            break;
        case IDC_EDLG_REFRESH:
            ExtensionManager::Instance().LoadInstalledExtensions();
            if (d && d->hList) PopulateExtensionsListView(d->hList);
            break;
        case IDC_EDLG_TOGGLE: {
            if (!d || !d->hList) break;
            int sel = ListView_GetNextItem(d->hList, -1, LVNI_SELECTED);
            const auto& exts = ExtensionManager::Instance().GetExtensions();
            if (sel >= 0 && sel < static_cast<int>(exts.size())) {
                ExtensionManager::Instance().SetExtensionEnabled(exts[sel].id, !exts[sel].isEnabled);
            }
            break;
        }
        case IDC_EDLG_RELOAD: {
            if (!d || !d->hList) break;
            int sel = ListView_GetNextItem(d->hList, -1, LVNI_SELECTED);
            const auto& exts = ExtensionManager::Instance().GetExtensions();
            if (sel >= 0 && sel < static_cast<int>(exts.size())) {
                ExtensionManager::Instance().ReloadExtension(exts[sel].id, [hWnd](bool ok) {
                    MessageBoxW(hWnd, ok ? L"扩展程序已重新加载！" : L"重新加载失败！", L"重新加载", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                });
            }
            break;
        }
        case IDC_EDLG_POPUP: {
            if (!d || !d->hList) break;
            int sel = ListView_GetNextItem(d->hList, -1, LVNI_SELECTED);
            const auto& exts = ExtensionManager::Instance().GetExtensions();
            if (sel >= 0 && sel < static_cast<int>(exts.size())) {
                RECT rect;
                GetWindowRect(d->hBtnPopup, &rect);
                ExtensionManager::Instance().ShowExtensionPopup(hWnd, exts[sel], POINT{ rect.left, rect.bottom });
            }
            break;
        }
        case IDC_EDLG_REMOVE: {
            if (!d || !d->hList) break;
            int sel = ListView_GetNextItem(d->hList, -1, LVNI_SELECTED);
            const auto& exts = ExtensionManager::Instance().GetExtensions();
            if (sel >= 0 && sel < static_cast<int>(exts.size())) {
                std::wstring confirmMsg = L"确定要移除扩展程序 [" + Utf8ToWide(exts[sel].name) + L"] 吗？";
                if (MessageBoxW(hWnd, confirmMsg.c_str(), L"移除扩展程序", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    ExtensionManager::Instance().RemoveExtension(exts[sel].id);
                }
            }
            break;
        }
        case IDC_EDLG_CLOSE:
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        auto* pnmh = reinterpret_cast<LPNMHDR>(lParam);
        if (pnmh && pnmh->idFrom == IDC_EDLG_LIST && pnmh->code == NM_DBLCLK) {
            if (d && d->hList) {
                int sel = ListView_GetNextItem(d->hList, -1, LVNI_SELECTED);
                const auto& exts = ExtensionManager::Instance().GetExtensions();
                if (sel >= 0 && sel < static_cast<int>(exts.size())) {
                    RECT rect;
                    GetWindowRect(hWnd, &rect);
                    ExtensionManager::Instance().ShowExtensionPopup(hWnd, exts[sel], POINT{ rect.left + 50, rect.top + 50 });
                }
            }
            return 0;
        }
        break;
    }

    case WM_DESTROY: {
        ExtensionManager::Instance().SetOnExtensionsChanged(nullptr);
        if (d) {
            if (d->hFont) DeleteObject(d->hFont);
            delete d;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        s_hExtensionsDlg = nullptr;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ExtensionManager::ShowExtensionsDialog(HWND hParent) {
    if (s_hExtensionsDlg && IsWindow(s_hExtensionsDlg)) {
        ShowWindow(s_hExtensionsDlg, SW_SHOW);
        SetForegroundWindow(s_hExtensionsDlg);
        return;
    }

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t DLG_CLASS_NAME[] = L"UltraLightExtensionsDlgClass";
    static bool s_dlgClassRegistered = false;
    if (!s_dlgClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ExtensionsDlgWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = DLG_CLASS_NAME;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(101));
        wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (RegisterClassExW(&wc)) {
            s_dlgClassRegistered = true;
        }
    }

    UINT dpi = GetDpiForWindow(hParent ? hParent : GetDesktopWindow());
    int dlgW = MulDiv(760, dpi, 96);
    int dlgH = MulDiv(520, dpi, 96);

    int posX = CW_USEDEFAULT;
    int posY = CW_USEDEFAULT;
    if (hParent && IsWindow(hParent)) {
        RECT prc;
        GetWindowRect(hParent, &prc);
        posX = prc.left + (prc.right - prc.left - dlgW) / 2;
        posY = prc.top + (prc.bottom - prc.top - dlgH) / 2;
    }

    s_hExtensionsDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        DLG_CLASS_NAME,
        L"Chrome 扩展程序管理 (Chrome Extensions)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_VISIBLE,
        posX, posY, dlgW, dlgH,
        hParent,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (s_hExtensionsDlg) {
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(s_hExtensionsDlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(s_hExtensionsDlg, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    }
}

} // namespace UltraLight
