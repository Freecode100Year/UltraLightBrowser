// User script store: metadata, URL rules, install/update, save/load.
// With a directory argument it also writes the injection scripts of a fixed set
// of scripts there, for tests/user_scripts_inject_test.cjs.

#include "UserScripts.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>

using namespace UltraLight;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } } while (0)

static bool Matches(const std::string& re, const std::string& url) {
    return !re.empty() && std::regex_search(url, std::regex(re, std::regex::ECMAScript));
}

static void TestRules() {
    const auto m = UserScriptStore::MatchToRegex;
    CHECK(Matches(m("*://*.example.com/*"), "https://example.com/"));
    CHECK(Matches(m("*://*.example.com/*"), "http://a.b.example.com/x?y"));
    CHECK(!Matches(m("*://*.example.com/*"), "https://badexample.com/"));
    CHECK(!Matches(m("*://*.example.com/*"), "https://example.com.evil.net/"));
    CHECK(Matches(m("https://example.com/path/*"), "https://example.com:8443/path/a"));
    CHECK(!Matches(m("https://example.com/path/*"), "http://example.com/path/a"));
    CHECK(!Matches(m("https://example.com/path/*"), "https://example.com/other"));
    CHECK(Matches(m("*://*/*"), "https://anything.test/"));
    CHECK(Matches(m("<all_urls>"), "http://x/"));
    CHECK(m("ftp://x/*").empty());
    CHECK(m("https://ex*ample.com/*").empty());
    CHECK(m("nonsense").empty());
    const auto i = UserScriptStore::IncludeToRegex;
    CHECK(Matches(i("*"), "https://a/"));
    CHECK(Matches(i("https://*.example.org/*"), "https://www.example.org/a"));
    CHECK(Matches(i("/^https:\\/\\/x\\.test\\//"), "https://x.test/1"));
    CHECK(!Matches(i("http://a.test/?q=1"), "http://a.test/xq=1"));  // ? is literal in a glob
}

static const char* kMeta = R"(// ==UserScript==
// @name         Hello
// @name:zh-CN   你好
// @namespace    https://example.com/ns
// @version      1.2
// @description  says hello
// @match        *://*.example.com/*
// @exclude      *://example.com/private/*
// @run-at       document-start
// @noframes
// @require      https://cdn.example.com/lib.js
// @grant        GM_addStyle
// ==/UserScript==
console.log("hi");
)";

static void TestMeta() {
    UserScript s;
    std::string error;
    CHECK(UserScriptStore::ParseMeta(kMeta, s, error));
    CHECK(error.empty());
    CHECK(s.name == "你好" && s.ns == "https://example.com/ns" && s.version == "1.2");
    CHECK(s.matches.size() == 1 && s.excludes.size() == 1 && s.runAt == "document-start" && s.noframes);
    CHECK(s.ignored.size() == 1 && s.ignored[0] == "@require");
    UserScript t;
    CHECK(!UserScriptStore::ParseMeta("alert(1)", t, error));
    UserScript u;
    error.clear();
    CHECK(UserScriptStore::ParseMeta("// ==UserScript==\n// @name x\n// ==/UserScript==\n", u, error));
    CHECK(!error.empty());  // no @match / @include
}

static void TestStore(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    UserScriptStore store(dir);
    const std::string a = store.Install(kMeta, "file.user.js", "");
    const std::string b = store.Install("document.title = 'x';", "plain.js", "https://plain.example/*");
    CHECK(a != b && store.Scripts().size() == 2);
    // Same @name + @namespace: replaced in place, keeps id and enabled state.
    CHECK(store.SetEnabled(a, false));
    std::string v2 = kMeta;
    v2.replace(v2.find("1.2"), 3, "1.3");
    CHECK(store.Install(v2, "", "") == a);
    CHECK(store.Scripts().size() == 2 && store.Scripts()[0].version == "1.3" && !store.Scripts()[0].enabled);
    bool threw = false;
    try { store.Install("x()", "bad.js", "not a rule"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { store.Install("// ==UserScript==\n// @name n\n// ==/UserScript==\n", "", ""); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(store.InjectionScripts().size() == 1);  // the disabled one is left out
    CHECK(store.Save());
    UserScriptStore again(dir);
    again.Load();
    CHECK(again.Scripts().size() == 2);
    CHECK(again.Scripts()[1].name == "plain.js" && again.Scripts()[1].matches.size() == 1);
    CHECK(again.InjectionScripts() == store.InjectionScripts());
    CHECK(again.Remove(b) && again.Save());
    CHECK(!std::filesystem::exists(dir / (b + ".user.js")));
    std::filesystem::remove_all(dir);
}

// Fixtures for the injection test.
static void WriteInjection(const std::filesystem::path& out) {
    const auto tmp = out / "store";
    std::filesystem::remove_all(tmp);
    UserScriptStore store(tmp);
    store.Install("// ==UserScript==\n// @name start\n// @match https://site.test/*\n// @run-at document-start\n// ==/UserScript==\n"
                  "window.__log.push('start:' + document.readyState); GM_setValue('n', {a: 1}); window.__log.push('get:' + GM_getValue('n').a);", "", "");
    store.Install("// ==UserScript==\n// @name idle\n// @match https://site.test/*\n// ==/UserScript==\n"
                  "window.__log.push('idle:' + document.readyState); GM_addStyle('body{color:red}'); window.__log.push('info:' + GM_info.script.name);", "", "");
    store.Install("// ==UserScript==\n// @name broken\n// @match https://site.test/*\n// ==/UserScript==\nthis is not javascript {", "", "");
    store.Install("// ==UserScript==\n// @name other\n// @match https://other.test/*\n// ==/UserScript==\nwindow.__log.push('other');", "", "");
    store.Install("// ==UserScript==\n// @name excluded\n// @match https://site.test/*\n// @exclude https://site.test/page*\n// ==/UserScript==\n"
                  "window.__log.push('excluded-ran');", "", "");
    store.Install("// ==UserScript==\n// @name noframes\n// @match https://site.test/*\n// @noframes\n// @run-at document-start\n// ==/UserScript==\n"
                  "window.__log.push('noframes');", "", "");
    store.Install("// ==UserScript==\n// @name thrower\n// @match https://site.test/*\n// @run-at document-start\n// ==/UserScript==\n"
                  "throw new Error('boom');", "", "");
    int n = 0;
    for (const auto& js : store.InjectionScripts()) std::ofstream(out / ("inject" + std::to_string(n++) + ".js"), std::ios::binary) << js;
    std::filesystem::remove_all(tmp);
}

int main(int argc, char** argv) {
    const auto dir = std::filesystem::temp_directory_path() / "ulb-userscripts-test";
    TestRules();
    TestMeta();
    TestStore(dir);
    if (argc > 1) WriteInjection(argv[1]);
    if (failures) return EXIT_FAILURE;
    std::cout << "user script tests passed\n";
    return 0;
}
