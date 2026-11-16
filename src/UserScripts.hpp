#pragma once

// User scripts ("用户脚本"): Tampermonkey-style .user.js files, or plain
// JavaScript with the sites to run on. Each enabled script becomes one
// document-created script for every tab (see InjectionScripts); it checks the
// page address itself and runs at its @run-at time with a small GM_* API.
// Plain data and files, no Windows calls, so it can be unit-tested.

#include <filesystem>
#include <string>
#include <vector>

namespace UltraLight {

struct UserScript {
    std::string id;
    std::string name;
    std::string ns;  // @namespace
    std::string version;
    std::string description;
    std::vector<std::string> matches;   // @match patterns
    std::vector<std::string> includes;  // @include globs or /regex/
    std::vector<std::string> excludes;  // @exclude and @exclude-match
    std::string runAt = "document-idle";
    bool noframes = false;
    bool enabled = true;
    std::vector<std::string> ignored;  // keys we do not support, e.g. "@require"
    std::string code;                  // the whole file
};

class UserScriptStore {
public:
    static constexpr size_t kMaxScripts = 200;
    static constexpr size_t kMaxSize = 2 * 1024 * 1024;

    explicit UserScriptStore(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    void Load();
    bool Save() const;
    const std::vector<UserScript>& Scripts() const { return m_scripts; }

    // Adds source, or replaces the script with the same @name and @namespace.
    // A file without a ==UserScript== block runs on defaultMatch and is named
    // fallbackName. Returns the id; throws std::runtime_error (Chinese message).
    std::string Install(const std::string& source, const std::string& fallbackName, const std::string& defaultMatch);
    bool Remove(const std::string& id);
    bool SetEnabled(const std::string& id, bool enabled);

    // One JavaScript source per enabled script, for AddScriptToExecuteOnDocumentCreated.
    std::vector<std::string> InjectionScripts() const;

    // Reads the ==UserScript== block. false when there is none; error is set when
    // there is one that cannot be used.
    static bool ParseMeta(const std::string& source, UserScript& out, std::string& error);
    // @match pattern / @include glob as an ECMAScript regex source ("" if invalid).
    static std::string MatchToRegex(const std::string& pattern);
    static std::string IncludeToRegex(const std::string& include);

private:
    std::string NextId() const;
    std::filesystem::path m_dir;
    std::vector<UserScript> m_scripts;
};

} // namespace UltraLight
