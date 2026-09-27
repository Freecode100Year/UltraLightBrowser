#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <mutex>

namespace UltraLight {

struct ExtensionConfigItem {
    std::string id;
    std::string name;
    std::wstring folderPath;
    bool enabled = true;
};

struct AppSettings {
    std::wstring startUrl = L"https://www.google.com";
    bool hardwareAcceleration = true;
    bool enableAdBlock = true;
    bool ecoMode = false;
    bool enablePublicDns = false;
    std::string selectedDnsProvider = "alidns";
    std::string customDnsTemplate = "";
    bool enableExtensions = true;
    bool preserveExtensionData = true;
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

    // Extension configuration persistence
    std::vector<ExtensionConfigItem> GetInstalledExtensions() const;
    void SetInstalledExtensions(const std::vector<ExtensionConfigItem>& exts);
    void AddOrUpdateExtensionConfig(const ExtensionConfigItem& item);
    void RemoveExtensionConfig(const std::string& id);
    void SetExtensionConfigEnabled(const std::string& id, bool enabled);

private:
    Config();
    ~Config() = default;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    std::filesystem::path m_configFilePath;
    AppSettings m_settings;
    std::unordered_map<std::string, std::vector<std::string>> m_hostBlockRules;
    std::vector<ExtensionConfigItem> m_extensionConfigs;
    mutable std::mutex m_mutex;
};

} // namespace UltraLight
