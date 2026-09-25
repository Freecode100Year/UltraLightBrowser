#include "Config.hpp"
#include "StringUtils.hpp"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <iostream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#else
// Minimal JSON fallback if nlohmann/json is not present
#endif

namespace UltraLight {

Config& Config::Instance() {
    static Config s_instance;
    return s_instance;
}

Config::Config() {
    PWSTR localAppDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppDataPath))) {
        std::filesystem::path basePath(localAppDataPath);
        CoTaskMemFree(localAppDataPath);
        std::filesystem::path appDir = basePath / "UltraLightBrowser";
        std::filesystem::create_directories(appDir);
        std::filesystem::create_directories(appDir / "Extensions");
        std::filesystem::create_directories(appDir / "UserData");
        m_configFilePath = appDir / "config.json";
    } else {
        m_configFilePath = "config.json";
    }
    Load();
}

std::filesystem::path Config::GetAppDataPath() const {
    return m_configFilePath.parent_path();
}

std::filesystem::path Config::GetExtensionsDirectory() const {
    return GetAppDataPath() / "Extensions";
}

std::filesystem::path Config::GetUserDataDirectory() const {
    return GetAppDataPath() / "UserData";
}

void Config::Load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!std::filesystem::exists(m_configFilePath)) {
        return;
    }

#if __has_include(<nlohmann/json.hpp>)
    try {
        std::ifstream file(m_configFilePath);
        if (!file.is_open()) return;

        json root;
        file >> root;

        if (root.contains("settings")) {
            auto& s = root["settings"];
            if (s.contains("startUrl") && s["startUrl"].is_string()) {
                m_settings.startUrl = StringUtils::Utf8ToWide(s["startUrl"].get<std::string>());
            }
            if (s.contains("hardwareAcceleration")) m_settings.hardwareAcceleration = s["hardwareAcceleration"];
            if (s.contains("enableExtensions")) m_settings.enableExtensions = s["enableExtensions"];
            if (s.contains("enableAdBlock")) m_settings.enableAdBlock = s["enableAdBlock"];
            if (s.contains("ecoMode")) m_settings.ecoMode = s["ecoMode"];
        }

        if (root.contains("blockRules") && root["blockRules"].is_object()) {
            m_hostBlockRules.clear();
            for (auto& [host, selectors] : root["blockRules"].items()) {
                if (selectors.is_array()) {
                    for (const auto& sel : selectors) {
                        m_hostBlockRules[host].push_back(sel.get<std::string>());
                    }
                }
            }
        }

        if (root.contains("unpackedExtensions") && root["unpackedExtensions"].is_array()) {
            m_unpackedExtensionPaths.clear();
            for (const auto& item : root["unpackedExtensions"]) {
                if (item.is_string()) {
                    std::string s = item.get<std::string>();
                    m_unpackedExtensionPaths.push_back(std::filesystem::path(StringUtils::Utf8ToWide(s)));
                }
            }
        }
    } catch (...) {
        // Fallback gracefully on parsing failure
    }
#endif
}

void Config::Save() {
    std::lock_guard<std::mutex> lock(m_mutex);

#if __has_include(<nlohmann/json.hpp>)
    try {
        json root;
        std::string startUrlNarrow = StringUtils::WideToUtf8(m_settings.startUrl);
        root["settings"] = {
            {"startUrl", startUrlNarrow},
            {"hardwareAcceleration", m_settings.hardwareAcceleration},
            {"enableExtensions", m_settings.enableExtensions},
            {"enableAdBlock", m_settings.enableAdBlock},
            {"ecoMode", m_settings.ecoMode}
        };

        json rulesObj = json::object();
        for (const auto& [host, selectors] : m_hostBlockRules) {
            rulesObj[host] = selectors;
        }
        root["blockRules"] = rulesObj;

        json extArr = json::array();
        for (const auto& p : m_unpackedExtensionPaths) {
            extArr.push_back(StringUtils::WideToUtf8(p.wstring()));
        }
        root["unpackedExtensions"] = extArr;

        std::filesystem::path tmpPath = m_configFilePath;
        tmpPath += L".tmp";
        {
            std::ofstream file(tmpPath);
            if (!file.is_open()) return;
            file << root.dump(4);
            file.flush();
        }
        std::error_code ec;
        std::filesystem::rename(tmpPath, m_configFilePath, ec);
        if (ec) {
            std::filesystem::copy_file(tmpPath, m_configFilePath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmpPath, ec);
        }
    } catch (...) {
        // Log or handle
    }
#endif
}

std::string Config::GetBlockRulesForHost(const std::string& host) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_hostBlockRules.find(host);
    if (it == m_hostBlockRules.end() || it->second.empty()) {
        return "";
    }

    std::ostringstream ss;
    for (const auto& sel : it->second) {
        if (!sel.empty()) {
            ss << sel << " { display: none !important; }\n";
        }
    }
    return ss.str();
}

std::unordered_map<std::string, std::vector<std::string>> Config::GetAllBlockRules() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hostBlockRules;
}

void Config::AddBlockRule(const std::string& host, const std::string& selector) {
    if (host.empty() || selector.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& list = m_hostBlockRules[host];
        // Deduplicate selector
        for (const auto& existing : list) {
            if (existing == selector) return;
        }
        // Limit max rules per host to 100 to prevent unbounded growth
        constexpr size_t MAX_RULES_PER_HOST = 100;
        if (list.size() >= MAX_RULES_PER_HOST) {
            return;
        }
        list.push_back(selector);
    }
    Save();
}

std::vector<std::filesystem::path> Config::GetUnpackedExtensionPaths() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_unpackedExtensionPaths;
}

void Config::AddUnpackedExtensionPath(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    auto canPath = std::filesystem::weakly_canonical(path, ec);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& p : m_unpackedExtensionPaths) {
            if (p == canPath || std::filesystem::equivalent(p, canPath, ec)) {
                return;
            }
        }
        m_unpackedExtensionPaths.push_back(canPath);
    }
    Save();
}

void Config::RemoveUnpackedExtensionPath(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    auto canPath = std::filesystem::weakly_canonical(path, ec);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::remove_if(m_unpackedExtensionPaths.begin(), m_unpackedExtensionPaths.end(), [&](const std::filesystem::path& p) {
            return p == canPath || p == path || std::filesystem::equivalent(p, canPath, ec);
        });
        if (it != m_unpackedExtensionPaths.end()) {
            m_unpackedExtensionPaths.erase(it, m_unpackedExtensionPaths.end());
        } else {
            return;
        }
    }
    Save();
}

} // namespace UltraLight
