#include <cctype>
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
            if (s.contains("userAgentProfile") && s["userAgentProfile"].is_string()) {
                const auto profile = s["userAgentProfile"].get<std::string>();
                m_settings.userAgentProfile = profile == "macos-edge" ? profile : "default";
            }
            if (s.contains("macPlatformVersion") && s["macPlatformVersion"].is_string()) {
                const auto v = s["macPlatformVersion"].get<std::string>();
                const bool valid = !v.empty() && v.size() <= 16 && std::isdigit(static_cast<unsigned char>(v.front())) &&
                    v.find_first_not_of("0123456789.") == std::string::npos;
                if (valid) m_settings.macPlatformVersion = v;
            }
            if (s.contains("hardwareAcceleration")) m_settings.hardwareAcceleration = s["hardwareAcceleration"];
            if (s.contains("enableAdBlock")) m_settings.enableAdBlock = s["enableAdBlock"];
            if (s.contains("ecoMode")) m_settings.ecoMode = s["ecoMode"];
            if (s.contains("enablePublicDns")) m_settings.enablePublicDns = s["enablePublicDns"];
            if (s.contains("selectedDnsProvider") && s["selectedDnsProvider"].is_string()) {
                m_settings.selectedDnsProvider = s["selectedDnsProvider"].get<std::string>();
            }
            if (s.contains("customDnsTemplate") && s["customDnsTemplate"].is_string()) {
                m_settings.customDnsTemplate = s["customDnsTemplate"].get<std::string>();
            }
            if (s.contains("systemAudioPassthrough") && s["systemAudioPassthrough"].is_boolean()) {
                m_settings.systemAudioPassthrough = s["systemAudioPassthrough"].get<bool>();
            }
            if (s.contains("enableSurroundSound")) m_settings.enableSurroundSound = s["enableSurroundSound"];
            if (s.contains("surroundSoundMode") && s["surroundSoundMode"].is_string()) {
                m_settings.surroundSoundMode = s["surroundSoundMode"].get<std::string>();
            }
            if (s.contains("audioDeviceMode") && s["audioDeviceMode"].is_string()) {
                m_settings.audioDeviceMode = s["audioDeviceMode"].get<std::string>();
            }
            if (s.contains("enableDeEsser") && s["enableDeEsser"].is_boolean()) m_settings.enableDeEsser = s["enableDeEsser"].get<bool>();
            if (s.contains("enableNightMode") && s["enableNightMode"].is_boolean()) m_settings.enableNightMode = s["enableNightMode"].get<bool>();
            if (s.contains("enableVocalBoost")) m_settings.enableVocalBoost = s["enableVocalBoost"];
            if (s.contains("audioVolumeBoost") && s["audioVolumeBoost"].is_number()) {
                m_settings.audioVolumeBoost = s["audioVolumeBoost"].get<double>();
            }
            if (s.contains("enableMonoDownmix")) m_settings.enableMonoDownmix = s["enableMonoDownmix"];

            auto readChoice = [&s](const char* key, std::string& target, std::initializer_list<const char*> allowed) {
                if (!s.contains(key) || !s[key].is_string()) return;
                const auto v = s[key].get<std::string>();
                for (const char* a : allowed) {
                    if (v == a) { target = v; return; }
                }
            };
            auto readBool = [&s](const char* key, bool& target) {
                if (s.contains(key) && s[key].is_boolean()) target = s[key].get<bool>();
            };
            auto readInt = [&s](const char* key, int& target, int lo, int hi) {
                if (s.contains(key) && s[key].is_number_integer()) {
                    const int v = s[key].get<int>();
                    if (v >= lo && v <= hi) target = v;
                }
            };
            readChoice("startupPage", m_settings.startupPage, {"start", "home"});
            readChoice("newTabPage", m_settings.newTabPage, {"start", "blank", "home"});
            readChoice("searchEngine", m_settings.searchEngine, {"google", "brave", "bing", "duckduckgo", "startpage", "baidu"});
            readBool("saveHistory", m_settings.saveHistory);
            readInt("tabSuspendMinutes", m_settings.tabSuspendMinutes, 0, 240);
            readBool("startShowFavorites", m_settings.startShowFavorites);
            readBool("startShowReading", m_settings.startShowReading);
            readChoice("startBackground", m_settings.startBackground, {"aurora", "ocean", "sunset", "plain"});
            readChoice("readerTheme", m_settings.readerTheme, {"light", "sepia", "gray", "dark"});
            readChoice("readerFont", m_settings.readerFont, {"serif", "sans"});
            readInt("readerFontSize", m_settings.readerFontSize, 12, 40);
            readBool("sidebarVisible", m_settings.sidebarVisible);
            readBool("preloadLinks", m_settings.preloadLinks);
            readBool("warpEnabled", m_settings.warpEnabled);
            readBool("warpFailClosed", m_settings.warpFailClosed);
            // v2.0.8: the start page became Brave Search. Move configs that still use
            // the old defaults (Google home, favorites page on start and new tabs).
            if (!s.contains("homeVersion")) {
                if (m_settings.startUrl == L"https://www.google.com") m_settings.startUrl = kDefaultHomeUrl;
                if (m_settings.startupPage == "start") m_settings.startupPage = "home";
                if (m_settings.newTabPage == "start") m_settings.newTabPage = "home";
            }
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
            {"homeVersion", 2},
            {"userAgentProfile", m_settings.userAgentProfile},
            {"macPlatformVersion", m_settings.macPlatformVersion},
            {"hardwareAcceleration", m_settings.hardwareAcceleration},
            {"enableAdBlock", m_settings.enableAdBlock},
            {"ecoMode", m_settings.ecoMode},
            {"enablePublicDns", m_settings.enablePublicDns},
            {"selectedDnsProvider", m_settings.selectedDnsProvider},
            {"customDnsTemplate", m_settings.customDnsTemplate},
            {"systemAudioPassthrough", m_settings.systemAudioPassthrough},
            {"enableSurroundSound", m_settings.enableSurroundSound},
            {"surroundSoundMode", m_settings.surroundSoundMode},
            {"audioDeviceMode", m_settings.audioDeviceMode},
            {"enableDeEsser", m_settings.enableDeEsser},
            {"enableNightMode", m_settings.enableNightMode},
            {"enableVocalBoost", m_settings.enableVocalBoost},
            {"audioVolumeBoost", m_settings.audioVolumeBoost},
            {"enableMonoDownmix", m_settings.enableMonoDownmix},
            {"startupPage", m_settings.startupPage},
            {"newTabPage", m_settings.newTabPage},
            {"searchEngine", m_settings.searchEngine},
            {"saveHistory", m_settings.saveHistory},
            {"tabSuspendMinutes", m_settings.tabSuspendMinutes},
            {"startShowFavorites", m_settings.startShowFavorites},
            {"startShowReading", m_settings.startShowReading},
            {"startBackground", m_settings.startBackground},
            {"readerTheme", m_settings.readerTheme},
            {"readerFont", m_settings.readerFont},
            {"readerFontSize", m_settings.readerFontSize},
            {"sidebarVisible", m_settings.sidebarVisible},
            {"preloadLinks", m_settings.preloadLinks},
            {"warpEnabled", m_settings.warpEnabled},
            {"warpFailClosed", m_settings.warpFailClosed}
        };

        json rulesObj = json::object();
        for (const auto& [host, selectors] : m_hostBlockRules) {
            rulesObj[host] = selectors;
        }
        root["blockRules"] = rulesObj;

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

void Config::ClearBlockRulesForHost(const std::string& host) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hostBlockRules.erase(host);
    }
    Save();
}

} // namespace UltraLight
