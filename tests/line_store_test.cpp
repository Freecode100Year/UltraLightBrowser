#include "LineStore.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace UltraLight;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } } while (0)

// Test values only (2001:db8::/32, 192.0.2.0/24, example.com).
static const std::string kVless = "vless://8f4c3a2e-1b2d-4c5e-9f6a-7b8c9d0e1f2a@[2001:db8::1]:443?security=reality&pbk=x&sid=1#%E9%A6%99%E6%B8%AF%201";
static const std::string kTrojan = "trojan://secret@example.com:8443?sni=example.com";
// base64 of {"v":"2","ps":"东京","add":"192.0.2.7","port":"443","id":"x"}
static const std::string kVmess = "vmess://eyJ2IjoiMiIsInBzIjoi5Lic5LqsIiwiYWRkIjoiMTkyLjAuMi43IiwicG9ydCI6IjQ0MyIsImlkIjoieCJ9";
// base64 of aes-256-gcm:pw@192.0.2.9:8388
static const std::string kSsLegacy = "ss://YWVzLTI1Ni1nY206cHdAMTkyLjAuMi45OjgzODg=#old";

static void TestDisplay() {
    CHECK(LineStore::NameOf(kVless) == "香港 1");
    CHECK(LineStore::ServerOf(kVless) == "[2001:db8::1]:443");
    CHECK(LineStore::ProtocolOf(kVless) == "VLESS");
    CHECK(LineStore::NameOf(kTrojan) == "Trojan example.com:8443");
    CHECK(LineStore::NameOf(kVmess) == "东京");
    CHECK(LineStore::ServerOf(kVmess) == "192.0.2.7:443");
    CHECK(LineStore::ServerOf(kSsLegacy) == "192.0.2.9:8388");
    CHECK(LineStore::ProtocolOf("hysteria2://pw@example.com:443") == "Hysteria2");
    CHECK(LineStore::ProtocolOf("hy2://pw@example.com:443") == "Hysteria2");
    CHECK(LineStore::ProtocolOf("wireguard://abc") == "WireGuard");
    CHECK(LineStore::NameOf("vless://x@h:1#%0A%0A") == "VLESS h:1");  // blank name falls back
}

static void TestAddSelectRemove() {
    LineStore s;
    CHECK(s.Selected() == LineStore::kPublic);
    CHECK(s.SelectedLinks().empty());
    CHECK(!s.Select(LineStore::kAuto));  // nothing to choose from yet
    CHECK(s.Add({kVless, " " + kTrojan + "\r", kVless, "", "not a link"}) == 2);
    CHECK(s.Lines().size() == 2);
    const std::string id = s.Lines()[1].id;
    CHECK(s.Select(id));
    CHECK(s.SelectedLinks().size() == 1 && s.SelectedLinks()[0] == kTrojan);
    CHECK(s.Rename(id, "  我的服务器\n "));
    CHECK(s.Find(id)->name == "我的服务器");
    CHECK(s.Rename(id, ""));
    CHECK(s.Find(id)->name == "Trojan example.com:8443");
    CHECK(s.Select(LineStore::kAuto));
    CHECK(s.SelectedLinks().size() == 2);
    CHECK(s.Select(id));
    CHECK(s.Remove(id));
    CHECK(s.Selected() == LineStore::kPublic);
    CHECK(!s.Select("l999"));
}

static void TestSubscriptions() {
    LineStore s;
    s.Add({kTrojan});
    const std::string sub = s.AddSub("机场", "https://sub.example.com/x");
    CHECK(s.AddSub("again", "https://sub.example.com/x") == sub);  // same URL, same subscription
    CHECK(s.ReplaceSub(sub, {kVless, kVmess}, 1000));
    CHECK(s.Lines().size() == 3 && s.Subs()[0].count == 2 && s.Subs()[0].updated == 1000);
    // Select a line of the subscription; it survives an update that still has it.
    std::string vmessId;
    for (const auto& l : s.Lines()) if (l.link == kVmess) vmessId = l.id;
    CHECK(s.Select(vmessId));
    CHECK(s.ReplaceSub(sub, {kVmess, kSsLegacy}, 2000));
    CHECK(s.SelectedLinks().size() == 1 && s.SelectedLinks()[0] == kVmess);
    // An update without it falls back to the public line.
    CHECK(s.ReplaceSub(sub, {kSsLegacy}, 3000));
    CHECK(s.Selected() == LineStore::kPublic);
    // Removing the subscription removes its lines, not the imported one.
    CHECK(s.RemoveSub(sub));
    CHECK(s.Lines().size() == 1 && s.Lines()[0].link == kTrojan && s.Subs().empty());
}

static void TestSaveLoad() {
    LineStore s;
    s.Add({kVless, kTrojan});
    const std::string sub = s.AddSub("", "https://sub.example.com/y");
    s.ReplaceSub(sub, {kVmess}, 42);
    s.Select(LineStore::kAuto);
    LineStore t;
    CHECK(t.Load(s.Save()));
    CHECK(t.Save() == s.Save());
    CHECK(t.Lines().size() == 3 && t.Subs().size() == 1 && t.Subs()[0].name == "订阅 1");
    CHECK(t.Selected() == LineStore::kAuto);
    // New ids never repeat old ones after a reload.
    t.Add({kSsLegacy});
    for (size_t i = 0; i + 1 < t.Lines().size(); ++i) CHECK(t.Lines()[i].id != t.Lines().back().id);
    CHECK(!t.Load("not json"));
    CHECK(t.Lines().empty() && t.Selected() == LineStore::kPublic);
    // A selection that points nowhere is dropped.
    CHECK(t.Load(R"({"lines":[],"selected":"l7"})"));
    CHECK(t.Selected() == LineStore::kPublic);
}

int main() {
    TestDisplay();
    TestAddSelectRemove();
    TestSubscriptions();
    TestSaveLoad();
    if (failures) return EXIT_FAILURE;
    std::cout << "line store tests passed\n";
    return 0;
}
