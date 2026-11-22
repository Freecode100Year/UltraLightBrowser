#pragma once

// Reading Chrome extension packages: manifest.json (with __MSG_..__ names from
// _locales) and the ZIP inside a .crx file. No Windows calls, so it can be
// unit-tested; Extensions.cpp does the installing.

#include <filesystem>
#include <string>

namespace UltraLight {

struct ExtensionManifest {
    std::string name;
    std::string version;
    std::string description;
    int manifestVersion = 0;
    std::string popup;    // action / browser_action / page_action default_popup
    std::string options;  // options_ui.page or options_page
    bool proxy = false;   // asks for the "proxy" permission: it can take over the browser's proxy
    std::string error;    // non-empty when the folder is not a usable extension
};

ExtensionManifest ReadExtensionManifest(const std::filesystem::path& dir);

// Where the ZIP starts in a .crx (version 2 or 3); 0 for a plain ZIP;
// std::string::npos for anything else.
size_t CrxZipOffset(const std::string& bytes);

// The relative page path of an extension, made safe for a chrome-extension:// URL
// ("" if it tries to leave the extension).
std::string ExtensionPagePath(const std::string& page);

} // namespace UltraLight
