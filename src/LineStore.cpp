#include "LineStore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace UltraLight {

using json = nlohmann::json;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Standard or URL-safe base64, padding optional; empty on bad input.
std::string DecodeBase64(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return {};
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i] == '+' ? ' ' : s[i]);
        }
    }
    return out;
}

std::string Scheme(const std::string& link) {
    const size_t p = link.find("://");
    return p == std::string::npos ? std::string() : Lower(link.substr(0, p));
}

json VmessJson(const std::string& link) {
    const std::string body = DecodeBase64(link.substr(link.find("://") + 3));
    json v = json::parse(body, nullptr, false);
    return v.is_object() ? v : json::object();
}

std::string JsonStr(const json& v, const char* key) {
    const auto it = v.find(key);
    if (it == v.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

// Strips characters that do not belong in a one-line label and caps the length.
std::string Clean(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\t'; }), s.end());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.size() > 80) {
        size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // keep UTF-8 whole
        s.resize(cut);
    }
    return s;
}

} // namespace

std::string LineStore::NameOf(const std::string& link) {
    if (Scheme(link) == "vmess") {
        const std::string ps = Clean(JsonStr(VmessJson(link), "ps"));
        if (!ps.empty()) return ps;
    } else if (const size_t hash = link.find('#'); hash != std::string::npos) {
        const std::string name = Clean(PercentDecode(link.substr(hash + 1)));
        if (!name.empty()) return name;
    }
    const std::string server = ServerOf(link);
    return server.empty() ? ProtocolOf(link) : ProtocolOf(link) + " " + server;
}

std::string LineStore::ProtocolOf(const std::string& link) {
    const std::string s = Scheme(link);
    if (s == "vless") return "VLESS";
    if (s == "trojan") return "Trojan";
    if (s == "vmess") return "VMess";
    if (s == "ss") return "Shadowsocks";
    if (s == "hysteria2" || s == "hy2") return "Hysteria2";
    if (s == "wireguard") return "WireGuard";
    return s;
}

std::string LineStore::ServerOf(const std::string& link) {
    const std::string s = Scheme(link);
    if (s.empty()) return {};
    if (s == "vmess") {
        const json v = VmessJson(link);
        const std::string host = JsonStr(v, "add"), port = JsonStr(v, "port");
        return host.empty() ? std::string() : host + ":" + port;
    }
    std::string rest = link.substr(link.find("://") + 3);
    rest = rest.substr(0, rest.find('#'));
    if (s == "ss" && rest.find('@') == std::string::npos) rest = DecodeBase64(rest.substr(0, rest.find('?')));
    const size_t at = rest.rfind('@');
    if (at == std::string::npos) return {};
    std::string hp = rest.substr(at + 1);
    hp = hp.substr(0, hp.find_first_of("/?"));
    return Clean(hp);
}

std::string LineStore::NextId(char prefix) {
    return std::string(1, prefix) + std::to_string(m_nextId++);
}

bool LineStore::Load(const std::string& text) {
    *this = LineStore();
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object()) return false;
    const auto str = [](const json& o, const char* k) {
        const auto it = o.find(k);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (const auto it = root.find("lines"); it != root.end() && it->is_array()) {
        for (const auto& l : *it) {
            if (!l.is_object()) continue;
            LineEntry e{str(l, "id"), str(l, "name"), str(l, "link"), str(l, "sub")};
            if (!e.id.empty() && !e.link.empty() && m_lines.size() < kMaxLines) m_lines.push_back(std::move(e));
        }
    }
    if (const auto it = root.find("subs"); it != root.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (!s.is_object()) continue;
            SubEntry e{str(s, "id"), str(s, "name"), str(s, "url")};
            e.updated = s.value("updated", std::int64_t{0});
            e.count = s.value("count", 0);
            if (!e.id.empty() && !e.url.empty()) m_subs.push_back(std::move(e));
        }
    }
    m_nextId = std::max<std::uint64_t>(1, root.value("next", std::uint64_t{1}));
    const std::string sel = str(root, "selected");
    m_selected = kPublic;
    if (!sel.empty()) Select(sel);
    return true;
}

std::string LineStore::Save() const {
    json lines = json::array();
    for (const auto& l : m_lines) lines.push_back({{"id", l.id}, {"name", l.name}, {"link", l.link}, {"sub", l.sub}});
    json subs = json::array();
    for (const auto& s : m_subs) subs.push_back({{"id", s.id}, {"name", s.name}, {"url", s.url}, {"updated", s.updated}, {"count", s.count}});
    return json{{"lines", lines}, {"subs", subs}, {"selected", m_selected}, {"next", m_nextId}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

const LineEntry* LineStore::Find(const std::string& id) const {
    for (const auto& l : m_lines) if (l.id == id) return &l;
    return nullptr;
}

size_t LineStore::Add(const std::vector<std::string>& links, const std::string& sub) {
    size_t added = 0;
    for (const auto& raw : links) {
        std::string link = raw;
        while (!link.empty() && std::isspace(static_cast<unsigned char>(link.back()))) link.pop_back();
        while (!link.empty() && std::isspace(static_cast<unsigned char>(link.front()))) link.erase(0, 1);
        if (link.empty() || link.size() > 8192 || Scheme(link).empty() || m_lines.size() >= kMaxLines) continue;
        const bool dup = std::any_of(m_lines.begin(), m_lines.end(), [&](const LineEntry& l) { return l.link == link; });
        if (dup) continue;
        m_lines.push_back({NextId('l'), NameOf(link), link, sub});
        ++added;
    }
    return added;
}

bool LineStore::Remove(const std::string& id) {
    const auto it = std::find_if(m_lines.begin(), m_lines.end(), [&](const LineEntry& l) { return l.id == id; });
    if (it == m_lines.end()) return false;
    m_lines.erase(it);
    if (m_selected == id) m_selected = kPublic;
    if (m_selected == kAuto && m_lines.empty()) m_selected = kPublic;
    return true;
}

bool LineStore::Rename(const std::string& id, const std::string& name) {
    for (auto& l : m_lines) {
        if (l.id == id) {
            const std::string n = Clean(name);
            l.name = n.empty() ? NameOf(l.link) : n;
            return true;
        }
    }
    return false;
}

bool LineStore::Select(const std::string& id) {
    if (id == kPublic || (id == kAuto && !m_lines.empty()) || Find(id)) {
        m_selected = id;
        return true;
    }
    return false;
}

std::string LineStore::AddSub(const std::string& name, const std::string& url) {
    for (const auto& s : m_subs) if (s.url == url) return s.id;
    SubEntry s{NextId('s'), Clean(name), url};
    if (s.name.empty()) s.name = "订阅 " + std::to_string(m_subs.size() + 1);
    m_subs.push_back(s);
    return s.id;
}

bool LineStore::RemoveSub(const std::string& id) {
    const auto it = std::find_if(m_subs.begin(), m_subs.end(), [&](const SubEntry& s) { return s.id == id; });
    if (it == m_subs.end()) return false;
    m_subs.erase(it);
    std::vector<std::string> doomed;
    for (const auto& l : m_lines) if (l.sub == id) doomed.push_back(l.id);
    for (const auto& d : doomed) Remove(d);
    return true;
}

bool LineStore::ReplaceSub(const std::string& id, const std::vector<std::string>& links, std::int64_t now) {
    auto sub = std::find_if(m_subs.begin(), m_subs.end(), [&](const SubEntry& s) { return s.id == id; });
    if (sub == m_subs.end()) return false;
    std::string keepLink;
    if (const LineEntry* sel = Find(m_selected); sel && sel->sub == id) keepLink = sel->link;
    const std::string selected = m_selected;
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(), [&](const LineEntry& l) { return l.sub == id; }), m_lines.end());
    Add(links, id);
    sub->updated = now;
    sub->count = static_cast<int>(std::count_if(m_lines.begin(), m_lines.end(), [&](const LineEntry& l) { return l.sub == id; }));
    if (!keepLink.empty()) {
        m_selected = kPublic;
        for (const auto& l : m_lines) if (l.link == keepLink) m_selected = l.id;
    } else if (!Select(selected)) {
        m_selected = kPublic;
    }
    return true;
}

std::vector<std::string> LineStore::SelectedLinks() const {
    std::vector<std::string> out;
    if (m_selected == kAuto) {
        for (const auto& l : m_lines) {
            if (out.size() >= kAutoMax) break;
            out.push_back(l.link);
        }
    } else if (const LineEntry* l = Find(m_selected)) {
        out.push_back(l->link);
    }
    return out;
}

} // namespace UltraLight
