#include "ExtensionPackage.hpp"
#include "RecommendedExtensions.hpp"

#include <set>

#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace UltraLight;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } } while (0)

static void Write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::string U32(std::uint32_t v) {
    return std::string{static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>(v >> 24)};
}

static void TestManifest(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    Write(dir / "manifest.json", "\xEF\xBB\xBF{\n // comment\n \"manifest_version\": 3, \"name\": \"__MSG_appName__\", \"version\": \"1.0.2\",\n"
                                 " \"default_locale\": \"en\", \"action\": {\"default_popup\": \"/popup/index.html\"},\n"
                                 " \"options_ui\": {\"page\": \"options.html\"}}");
    Write(dir / "_locales/en/messages.json", R"({"APPNAME": {"message": "Blocker"}})");
    Write(dir / "_locales/zh_CN/messages.json", R"({"appName": {"message": "拦截器"}})");
    ExtensionManifest m = ReadExtensionManifest(dir);
    CHECK(m.error.empty());
    CHECK(m.name == "拦截器" && m.version == "1.0.2" && m.manifestVersion == 3);
    CHECK(m.popup == "popup/index.html" && m.options == "options.html");
    std::filesystem::remove(dir / "_locales/zh_CN/messages.json");
    CHECK(ReadExtensionManifest(dir).name == "Blocker");  // default locale, key case-insensitive
    Write(dir / "manifest.json", R"({"manifest_version": 2, "name": "Old", "version": "2", "browser_action": {"default_popup": "../x.html"}, "options_page": "o.html"})");
    m = ReadExtensionManifest(dir);
    CHECK(m.error.empty() && m.popup.empty() && m.options == "o.html");
    Write(dir / "manifest.json", R"({"manifest_version": 1, "name": "x", "version": "1"})");
    CHECK(!ReadExtensionManifest(dir).error.empty());
    std::filesystem::remove(dir / "manifest.json");
    CHECK(!ReadExtensionManifest(dir).error.empty());
    std::filesystem::remove_all(dir);
}

static void TestCrx() {
    const std::string zip = std::string("PK\x03\x04", 4) + "rest";
    CHECK(CrxZipOffset(zip) == 0);
    CHECK(CrxZipOffset("Cr24" + U32(3) + U32(5) + "hdr12" + zip) == 17);
    CHECK(CrxZipOffset("Cr24" + U32(2) + U32(3) + U32(2) + "keysg" + zip) == 21);
    CHECK(CrxZipOffset("Cr24" + U32(3) + U32(500) + zip) == std::string::npos);  // header runs past the end
    CHECK(CrxZipOffset("Cr24" + U32(4) + U32(0) + zip) == std::string::npos);
    CHECK(CrxZipOffset("<html>") == std::string::npos);
}

static void TestProxyPermission(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    Write(dir / "manifest.json", R"({"manifest_version": 3, "name": "V", "version": "1", "permissions": ["storage", "proxy"]})");
    CHECK(ReadExtensionManifest(dir).proxy);
    Write(dir / "manifest.json", R"({"manifest_version": 3, "name": "V", "version": "1", "optional_permissions": ["proxy"]})");
    CHECK(ReadExtensionManifest(dir).proxy);
    Write(dir / "manifest.json", R"({"manifest_version": 3, "name": "V", "version": "1", "permissions": ["storage", "proxyish"]})");
    CHECK(!ReadExtensionManifest(dir).proxy);
    std::filesystem::remove_all(dir);
}

static void TestRecommended() {
    std::set<std::string> keys;
    for (const auto& r : RecommendedExtensions()) {
        CHECK(keys.insert(r.key).second);
        CHECK(FindRecommendedExtension(r.key) == &r);
        CHECK(std::string(r.homepage).rfind("https://", 0) == 0);
        const std::string id = r.storeId, url = r.url, sum = r.sha256;
        // Either a store id (32 letters a-p) or an https ZIP with its SHA-256, never both.
        CHECK(id.empty() != url.empty());
        if (!id.empty()) {
            CHECK(id.size() == 32 && id.find_first_not_of("abcdefghijklmnop") == std::string::npos);
            CHECK(StoreDownloadUrl(id).find("x=id%3D" + id + "%26uc") != std::string::npos);
        } else {
            CHECK(url.rfind("https://", 0) == 0 && sum.size() == 64 && sum.find_first_not_of("0123456789abcdef") == std::string::npos);
        }
        // A proxy-taking extension must say so before it is installed.
        if (r.proxy) CHECK(std::string(r.note).find("代理") != std::string::npos);
    }
    CHECK(FindRecommendedExtension("hola") == nullptr);
    CHECK(FindRecommendedExtension("") == nullptr);
}

static void TestPagePath() {
    CHECK(ExtensionPagePath("/a/b.html") == "a/b.html");
    CHECK(ExtensionPagePath("a\\b.html") == "a/b.html");
    CHECK(ExtensionPagePath("a/../../b.html").empty());
    CHECK(ExtensionPagePath("https://evil/").empty());
    CHECK(ExtensionPagePath("").empty());
}

int main() {
    TestManifest(std::filesystem::temp_directory_path() / "ulb-ext-test");
    TestCrx();
    TestPagePath();
    TestProxyPermission(std::filesystem::temp_directory_path() / "ulb-ext-proxy-test");
    TestRecommended();
    if (failures) return EXIT_FAILURE;
    std::cout << "extension package tests passed\n";
    return 0;
}
