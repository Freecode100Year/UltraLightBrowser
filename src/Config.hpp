#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <mutex>

namespace UltraLight {

struct AppSettings {
    std::wstring startUrl = L"https://www.google.com";
    std::string userAgentProfile = "default"; // default / macos-edge
    std::string macPlatformVersion = "26.2.0"; // Sec-CH-UA-Platform-Version in macOS mode
    bool hardwareAcceleration = true;
    bool enableAdBlock = true;
    bool ecoMode = false;
    bool enablePublicDns = false;
    std::string selectedDnsProvider = "quad9";
    std::string customDnsTemplate = "";
    bool systemAudioPassthrough = true;       // Keep original output for Windows spatial audio.
    bool enableSurroundSound = true;
    std::string surroundSoundMode = "standard"; // "light", "standard", "cinema"
    std::string audioDeviceMode = "auto";       // "auto", "headphones", "speakers"
    bool enableDeEsser = false;
    bool enableNightMode = false;
    bool enableVocalBoost = false;
    double audioVolumeBoost = 1.0;              // 1.0, 1.5, 2.0, 3.0
    bool enableMonoDownmix = false;

    // Browser (v2.0)
    std::string startupPage = "start";          // start / home
    std::string newTabPage = "start";           // start / blank / home
    std::string searchEngine = "google";        // google / bing / duckduckgo / startpage / baidu
    bool saveHistory = true;
    int tabSuspendMinutes = 10;                 // 0 = never
    bool startShowFavorites = true;
    bool startShowReading = true;
    std::string startBackground = "aurora";     // aurora / ocean / sunset / plain
    std::string readerTheme = "sepia";          // light / sepia / gray / dark
    std::string readerFont = "serif";           // serif / sans
    int readerFontSize = 19;
    bool sidebarVisible = false;
    bool preloadLinks = true;                   // prefetch same-site links on hover
};

class Config {
public:
    static Config& Instance();

    void Load();
    void Save();

    std::filesystem::path GetAppDataPath() const;
    std::filesystem::path GetUserDataDirectory() const;

    AppSettings& GetSettings() { return m_settings; }
    const AppSettings& GetSettings() const { return m_settings; }

    std::string GetBlockRulesForHost(const std::string& host);
    std::unordered_map<std::string, std::vector<std::string>> GetAllBlockRules() const;
    void AddBlockRule(const std::string& host, const std::string& selector);
    void ClearBlockRulesForHost(const std::string& host);

private:
    Config();
    ~Config() = default;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    std::filesystem::path m_configFilePath;
    AppSettings m_settings;
    std::unordered_map<std::string, std::vector<std::string>> m_hostBlockRules;
    mutable std::mutex m_mutex;
};

} // namespace UltraLight
