#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <mutex>

namespace UltraLight {

struct AppSettings {
    std::wstring startUrl = L"https://www.google.com";
    bool hardwareAcceleration = true;
    bool enableAdBlock = true;
    bool ecoMode = false;
    bool enablePublicDns = false;
    std::string selectedDnsProvider = "alidns";
    std::string customDnsTemplate = "";
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
