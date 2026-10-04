#include "Library.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace UltraLight {

namespace {

constexpr std::size_t kMaxHistory = 5000;
constexpr std::int64_t kHistoryRetentionMs = 90LL * 24 * 3600 * 1000;
constexpr std::size_t kMaxTitle = 512;
constexpr std::size_t kMaxUrl = 8192;

std::string Clip(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t cut = max;
    // Do not split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string Lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ReadJson(const std::filesystem::path& path, json& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    try {
        in >> out;
        return true;
    } catch (...) {
        return false;
    }
}

std::string Str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::int64_t Int(const json& obj, const char* key) {
    if (!obj.is_object()) return 0;
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<std::int64_t>() : 0;
}

bool Bool(const json& obj, const char* key, bool fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string HtmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

void AppendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x110000) { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

std::string HtmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent == "nbsp") out += ' ';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    ? std::stoul(ent.substr(2), nullptr, 16) : std::stoul(ent.substr(1));
            } catch (...) { out += s.substr(i, semi - i + 1); i = semi; continue; }
            AppendUtf8(out, cp);
        } else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Case-insensitive search for an ASCII tag/attribute name.
std::size_t FindCI(const std::string& hay, const std::string& needle, std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char a = hay[i + j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != needle[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return std::string::npos;
}

std::string AttrValue(const std::string& tag, const std::string& name) {
    auto pos = FindCI(tag, name + "=", 0);
    if (pos == std::string::npos) return {};
    pos += name.size() + 1;
    if (pos >= tag.size()) return {};
    const char quote = tag[pos];
    if (quote == '"' || quote == '\'') {
        const auto end = tag.find(quote, pos + 1);
        if (end == std::string::npos) return {};
        return tag.substr(pos + 1, end - pos - 1);
    }
    const auto end = tag.find_first_of(" >", pos);
    return tag.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

} // namespace

bool SiteSettings::IsDefault() const {
    return zoom == 0.0 && !autoReader && camera == "ask" && microphone == "ask" &&
           location == "ask" && popups == "block" && adblock;
}

Library::Library(std::filesystem::path directory) : m_dir(std::move(directory)) {
    m_folders = {kFavoritesFolder, kBookmarksFolder};
}

std::string Library::HostOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    auto start = scheme + 3;
    auto end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(start, end - start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        return Lower(authority.substr(0, close == std::string::npos ? authority.size() : close + 1));
    }
    const auto colon = authority.find(':');
    if (colon != std::string::npos) authority = authority.substr(0, colon);
    while (!authority.empty() && authority.back() == '.') authority.pop_back();
    return Lower(authority);
}

bool Library::IsWebUrl(const std::string& url) {
    const std::string lower = Lower(url.substr(0, 8));
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

std::string Library::NewId(std::int64_t now) {
    std::ostringstream ss;
    ss << std::hex << now << '-' << ++m_idCounter;
    return ss.str();
}

// ---------------------------------------------------------------- persistence

void Library::Load() {
    m_dirtyLibrary = false;
    LoadLibraryFile();
    // History, site settings and the old session / privacy stores only ever live in
    // memory; files left by earlier versions are removed.
    std::error_code ec;
    for (const char* name : {"history.json", "privacy.json", "session.json"}) {
        std::filesystem::remove(m_dir / name, ec);
        std::filesystem::remove(m_dir / (std::string(name) + ".tmp"), ec);
    }
}

void Library::LoadLibraryFile() {
    json root;
    if (!ReadJson(m_dir / "library.json", root) || !root.is_object()) return;
    if (root.contains("folders") && root["folders"].is_array()) {
        for (const auto& f : root["folders"]) {
            if (f.is_string()) AddFolder(f.get<std::string>());
        }
    }
    if (root.contains("bookmarks") && root["bookmarks"].is_array()) {
        for (const auto& b : root["bookmarks"]) {
            Bookmark bm{Str(b, "id"), Clip(Str(b, "title"), kMaxTitle), Clip(Str(b, "url"), kMaxUrl), Str(b, "folder"), Int(b, "added")};
            if (bm.id.empty() || bm.url.empty()) continue;
            if (std::find(m_folders.begin(), m_folders.end(), bm.folder) == m_folders.end()) bm.folder = kBookmarksFolder;
            m_bookmarks.push_back(std::move(bm));
        }
    }
    if (root.contains("reading") && root["reading"].is_array()) {
        for (const auto& r : root["reading"]) {
            ReadingItem item{Str(r, "id"), Clip(Str(r, "title"), kMaxTitle), Clip(Str(r, "url"), kMaxUrl), Int(r, "added"), Bool(r, "read", false)};
            if (!item.id.empty() && !item.url.empty()) m_reading.push_back(std::move(item));
        }
    }
    if (root.contains("groups") && root["groups"].is_array()) {
        for (const auto& g : root["groups"]) {
            TabGroup group{Str(g, "id"), Clip(Str(g, "name"), kMaxTitle), {}, Int(g, "updated")};
            if (g.contains("tabs") && g["tabs"].is_array()) {
                for (const auto& t : g["tabs"]) {
                    SavedTab tab{Clip(Str(t, "title"), kMaxTitle), Clip(Str(t, "url"), kMaxUrl)};
                    if (!tab.url.empty()) group.tabs.push_back(std::move(tab));
                }
            }
            if (!group.id.empty()) m_groups.push_back(std::move(group));
        }
    }
    // Older versions saved per-site settings (a list of visited hosts); rewrite without them.
    if (root.contains("sites")) m_dirtyLibrary = true;
}

bool Library::WriteFileAtomic(const std::filesystem::path& path, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << data;
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (!ec) return true;
    std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
    const bool copied = !ec;
    std::filesystem::remove(tmp, ec);
    return copied;
}

bool Library::Save() {
    bool ok = true;
    for (const auto& [path, data] : TakeSnapshot()) ok = WriteFileAtomic(path, data) && ok;
    if (!ok) m_dirtyLibrary = true;
    return ok;
}

std::vector<std::pair<std::filesystem::path, std::string>> Library::TakeSnapshot() {
    std::vector<std::pair<std::filesystem::path, std::string>> out;
    if (m_dirtyLibrary) {
        json root;
        root["folders"] = m_folders;
        json bms = json::array();
        for (const auto& b : m_bookmarks) bms.push_back({{"id", b.id}, {"title", b.title}, {"url", b.url}, {"folder", b.folder}, {"added", b.added}});
        root["bookmarks"] = std::move(bms);
        json rl = json::array();
        for (const auto& r : m_reading) rl.push_back({{"id", r.id}, {"title", r.title}, {"url", r.url}, {"added", r.added}, {"read", r.read}});
        root["reading"] = std::move(rl);
        json groups = json::array();
        for (const auto& g : m_groups) {
            json tabs = json::array();
            for (const auto& t : g.tabs) tabs.push_back({{"title", t.title}, {"url", t.url}});
            groups.push_back({{"id", g.id}, {"name", g.name}, {"updated", g.updated}, {"tabs", std::move(tabs)}});
        }
        root["groups"] = std::move(groups);
        out.emplace_back(m_dir / "library.json", root.dump(-1, ' ', false, json::error_handler_t::replace));
        m_dirtyLibrary = false;
    }
    return out;
}

// ---------------------------------------------------------------- bookmarks

bool Library::AddFolder(const std::string& rawName) {
    const std::string name = Clip(Trim(rawName), 128);
    if (name.empty() || std::find(m_folders.begin(), m_folders.end(), name) != m_folders.end()) return false;
    m_folders.push_back(name);
    m_dirtyLibrary = true;
    return true;
}

bool Library::RenameFolder(const std::string& from, const std::string& rawTo) {
    const std::string to = Clip(Trim(rawTo), 128);
    if (from == kFavoritesFolder || from == kBookmarksFolder || to.empty()) return false;
    auto it = std::find(m_folders.begin(), m_folders.end(), from);
    if (it == m_folders.end() || std::find(m_folders.begin(), m_folders.end(), to) != m_folders.end()) return false;
    *it = to;
    for (auto& b : m_bookmarks) if (b.folder == from) b.folder = to;
    m_dirtyLibrary = true;
    return true;
}

bool Library::RemoveFolder(const std::string& name) {
    if (name == kFavoritesFolder || name == kBookmarksFolder) return false;
    auto it = std::find(m_folders.begin(), m_folders.end(), name);
    if (it == m_folders.end()) return false;
    m_folders.erase(it);
    for (auto& b : m_bookmarks) if (b.folder == name) b.folder = kBookmarksFolder;
    m_dirtyLibrary = true;
    return true;
}

std::vector<Bookmark> Library::BookmarksIn(const std::string& folder) const {
    std::vector<Bookmark> out;
    for (const auto& b : m_bookmarks) if (b.folder == folder) out.push_back(b);
    return out;
}

std::string Library::AddBookmark(const std::string& title, const std::string& url, const std::string& folder, std::int64_t now) {
    if (url.empty() || url.size() > kMaxUrl) return {};
    Bookmark b;
    b.id = NewId(now);
    b.title = Clip(Trim(title).empty() ? url : Trim(title), kMaxTitle);
    b.url = url;
    b.folder = std::find(m_folders.begin(), m_folders.end(), folder) != m_folders.end() ? folder : kBookmarksFolder;
    b.added = now;
    m_bookmarks.push_back(b);
    m_dirtyLibrary = true;
    return b.id;
}

bool Library::UpdateBookmark(const std::string& id, const std::string& title, const std::string& url, const std::string& folder) {
    for (auto& b : m_bookmarks) {
        if (b.id != id) continue;
        if (!url.empty() && url.size() <= kMaxUrl) b.url = url;
        if (!Trim(title).empty()) b.title = Clip(Trim(title), kMaxTitle);
        if (std::find(m_folders.begin(), m_folders.end(), folder) != m_folders.end()) b.folder = folder;
        m_dirtyLibrary = true;
        return true;
    }
    return false;
}

bool Library::RemoveBookmark(const std::string& id) {
    const auto it = std::find_if(m_bookmarks.begin(), m_bookmarks.end(), [&](const Bookmark& b) { return b.id == id; });
    if (it == m_bookmarks.end()) return false;
    m_bookmarks.erase(it);
    m_dirtyLibrary = true;
    return true;
}

bool Library::MoveBookmark(const std::string& id, const std::string& beforeId) {
    const auto it = std::find_if(m_bookmarks.begin(), m_bookmarks.end(), [&](const Bookmark& b) { return b.id == id; });
    if (it == m_bookmarks.end() || id == beforeId) return false;
    Bookmark moved = *it;
    m_bookmarks.erase(it);
    const auto target = std::find_if(m_bookmarks.begin(), m_bookmarks.end(), [&](const Bookmark& b) { return b.id == beforeId; });
    if (target != m_bookmarks.end()) moved.folder = target->folder;
    m_bookmarks.insert(target, std::move(moved));
    m_dirtyLibrary = true;
    return true;
}

const Bookmark* Library::FindBookmarkByUrl(const std::string& url) const {
    for (const auto& b : m_bookmarks) if (b.url == url) return &b;
    return nullptr;
}

int Library::ImportNetscapeHtml(const std::string& html, std::int64_t now) {
    // Walk <H3> folder headings and <A HREF> links; nested folders are flattened to
    // the innermost heading, top-level links go to the default folder.
    std::vector<std::string> folderStack;
    std::string pendingFolder;
    std::set<std::string> existing;
    for (const auto& b : m_bookmarks) existing.insert(b.folder + "\n" + b.url);
    int imported = 0;
    std::size_t pos = 0;
    while (pos < html.size()) {
        const auto lt = html.find('<', pos);
        if (lt == std::string::npos) break;
        const auto gt = html.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = html.substr(lt + 1, gt - lt - 1);
        std::string name = Lower(tag.substr(0, tag.find_first_of(" \t\r\n")));
        pos = gt + 1;
        if (name == "h3") {
            const auto close = FindCI(html, "</h3>", pos);
            if (close == std::string::npos) break;
            pendingFolder = Clip(Trim(HtmlUnescape(html.substr(pos, close - pos))), 128);
            pos = close + 5;
        } else if (name == "dl") {
            folderStack.push_back(pendingFolder);
            pendingFolder.clear();
        } else if (name == "/dl") {
            if (!folderStack.empty()) folderStack.pop_back();
        } else if (name == "a") {
            const auto close = FindCI(html, "</a>", pos);
            if (close == std::string::npos) break;
            const std::string url = HtmlUnescape(AttrValue(tag, "href"));
            const std::string title = Trim(HtmlUnescape(html.substr(pos, close - pos)));
            pos = close + 4;
            if (!IsWebUrl(url)) continue;
            std::string folder = kBookmarksFolder;
            for (auto it = folderStack.rbegin(); it != folderStack.rend(); ++it) {
                if (!it->empty()) { folder = *it; break; }
            }
            const std::string lower = Lower(folder);
            if (lower == "favorites bar" || lower == "bookmarks bar" || lower == "收藏夹栏" ||
                lower == "书签栏" || lower == "favorites" || lower == "个人收藏") {
                folder = kFavoritesFolder;
            }
            AddFolder(folder);
            if (!existing.insert(folder + "\n" + url).second) continue;
            AddBookmark(title, url, folder, now);
            ++imported;
        }
    }
    return imported;
}

std::string Library::ExportNetscapeHtml() const {
    std::ostringstream ss;
    ss << "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
          "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
          "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    for (const auto& folder : m_folders) {
        const bool favorites = folder == kFavoritesFolder;
        ss << "    <DT><H3" << (favorites ? " PERSONAL_TOOLBAR_FOLDER=\"true\"" : "") << ">"
           << HtmlEscape(favorites ? std::string("Favorites Bar") : folder) << "</H3>\n    <DL><p>\n";
        for (const auto& b : m_bookmarks) {
            if (b.folder != folder) continue;
            ss << "        <DT><A HREF=\"" << HtmlEscape(b.url) << "\" ADD_DATE=\"" << (b.added / 1000) << "\">"
               << HtmlEscape(b.title) << "</A>\n";
        }
        ss << "    </DL><p>\n";
    }
    ss << "</DL><p>\n";
    return ss.str();
}

// ---------------------------------------------------------------- reading list

std::string Library::AddReading(const std::string& title, const std::string& url, std::int64_t now) {
    if (url.empty() || url.size() > kMaxUrl) return {};
    for (auto& r : m_reading) {
        if (r.url == url) {
            r.read = false;
            r.added = now;
            m_dirtyLibrary = true;
            return r.id;
        }
    }
    ReadingItem item{NewId(now), Clip(Trim(title).empty() ? url : Trim(title), kMaxTitle), url, now, false};
    m_reading.insert(m_reading.begin(), item);
    m_dirtyLibrary = true;
    return item.id;
}

bool Library::RemoveReading(const std::string& id) {
    const auto it = std::find_if(m_reading.begin(), m_reading.end(), [&](const ReadingItem& r) { return r.id == id; });
    if (it == m_reading.end()) return false;
    m_reading.erase(it);
    m_dirtyLibrary = true;
    return true;
}

bool Library::SetRead(const std::string& id, bool read) {
    for (auto& r : m_reading) {
        if (r.id == id) {
            if (r.read != read) { r.read = read; m_dirtyLibrary = true; }
            return true;
        }
    }
    return false;
}

void Library::MarkReadByUrl(const std::string& url) {
    for (auto& r : m_reading) {
        if (r.url == url && !r.read) { r.read = true; m_dirtyLibrary = true; }
    }
}

// ---------------------------------------------------------------- history

void Library::RecordVisit(const std::string& url, const std::string& title, std::int64_t now) {
    if (!IsWebUrl(url) || url.size() > kMaxUrl) return;
    const auto it = m_historyIndex.find(url);
    if (it != m_historyIndex.end()) {
        auto& e = m_history[it->second];
        e.last = now;
        ++e.visits;
        if (!title.empty()) e.title = Clip(title, kMaxTitle);
    } else {
        m_historyIndex[url] = m_history.size();
        m_history.push_back({url, Clip(title, kMaxTitle), now, 1});
        if (m_history.size() > kMaxHistory + kMaxHistory / 10) PruneHistory();
    }
}

void Library::UpdateTitle(const std::string& url, const std::string& title) {
    const auto it = m_historyIndex.find(url);
    if (it == m_historyIndex.end() || title.empty()) return;
    auto& e = m_history[it->second];
    const std::string clipped = Clip(title, kMaxTitle);
    if (e.title != clipped) {
        e.title = clipped;
    }
}

void Library::PruneHistory() {
    std::sort(m_history.begin(), m_history.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.last > b.last; });
    if (!m_history.empty()) {
        const std::int64_t newest = m_history.front().last;
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
            [newest](const HistoryEntry& e) { return newest - e.last > kHistoryRetentionMs; }), m_history.end());
    }
    if (m_history.size() > kMaxHistory) m_history.resize(kMaxHistory);
    m_historyIndex.clear();
    for (std::size_t i = 0; i < m_history.size(); ++i) m_historyIndex[m_history[i].url] = i;
}

std::vector<HistoryEntry> Library::QueryHistory(const std::string& query, std::size_t limit) const {
    const std::string q = Lower(Trim(query));
    std::vector<HistoryEntry> out;
    for (const auto& e : m_history) {
        if (q.empty() || Lower(e.title).find(q) != std::string::npos || Lower(e.url).find(q) != std::string::npos) {
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.last > b.last; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

bool Library::RemoveHistory(const std::string& url) {
    const auto it = m_historyIndex.find(url);
    if (it == m_historyIndex.end()) return false;
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(it->second));
    m_historyIndex.clear();
    for (std::size_t i = 0; i < m_history.size(); ++i) m_historyIndex[m_history[i].url] = i;
    return true;
}

void Library::ClearHistory(std::int64_t since) {
    if (since <= 0) {
        m_history.clear();
    } else {
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
            [since](const HistoryEntry& e) { return e.last >= since; }), m_history.end());
    }
    m_historyIndex.clear();
    for (std::size_t i = 0; i < m_history.size(); ++i) m_historyIndex[m_history[i].url] = i;
}

std::vector<Suggestion> Library::Suggest(const std::string& query, std::size_t limit) const {
    const std::string q = Lower(Trim(query));
    if (q.empty()) return {};
    struct Scored { Suggestion s; double score; };
    std::vector<Scored> scored;
    std::set<std::string> seen;
    auto stripped = [](const std::string& url) {
        std::string u = Lower(url);
        for (const char* p : {"https://", "http://"}) if (u.rfind(p, 0) == 0) { u = u.substr(std::string(p).size()); break; }
        if (u.rfind("www.", 0) == 0) u = u.substr(4);
        return u;
    };
    auto score = [&](const std::string& title, const std::string& url, double weight) {
        const std::string u = stripped(url);
        const std::string t = Lower(title);
        double s = 0;
        if (u.rfind(q, 0) == 0) s = 3;
        else if (HostOf(url).find(q) != std::string::npos) s = 2;
        else if (u.find(q) != std::string::npos || t.find(q) != std::string::npos) s = 1;
        return s * weight;
    };
    for (const auto& b : m_bookmarks) {
        const double s = score(b.title, b.url, 1.5);
        if (s > 0 && seen.insert(b.url).second) scored.push_back({{b.title, b.url, true}, s + 0.5});
    }
    for (const auto& e : m_history) {
        double s = score(e.title, e.url, 1.0);
        if (s <= 0 || !seen.insert(e.url).second) continue;
        s += std::min(1.0, e.visits / 20.0);
        scored.push_back({{e.title, e.url, false}, s});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    std::vector<Suggestion> out;
    for (auto& s : scored) {
        if (out.size() >= limit) break;
        out.push_back(std::move(s.s));
    }
    return out;
}

// ---------------------------------------------------------------- tab groups

std::string Library::SaveGroup(const std::string& name, const std::vector<SavedTab>& tabs, std::int64_t now) {
    TabGroup g;
    g.id = NewId(now);
    g.name = Clip(Trim(name).empty() ? std::string("标签组") : Trim(name), kMaxTitle);
    for (const auto& t : tabs) {
        if (IsWebUrl(t.url)) g.tabs.push_back({Clip(t.title, kMaxTitle), Clip(t.url, kMaxUrl)});
    }
    if (g.tabs.empty()) return {};
    g.updated = now;
    m_groups.push_back(g);
    m_dirtyLibrary = true;
    return g.id;
}

bool Library::RenameGroup(const std::string& id, const std::string& name) {
    for (auto& g : m_groups) {
        if (g.id == id && !Trim(name).empty()) {
            g.name = Clip(Trim(name), kMaxTitle);
            m_dirtyLibrary = true;
            return true;
        }
    }
    return false;
}

bool Library::RemoveGroup(const std::string& id) {
    const auto it = std::find_if(m_groups.begin(), m_groups.end(), [&](const TabGroup& g) { return g.id == id; });
    if (it == m_groups.end()) return false;
    m_groups.erase(it);
    m_dirtyLibrary = true;
    return true;
}

const TabGroup* Library::FindGroup(const std::string& id) const {
    for (const auto& g : m_groups) if (g.id == id) return &g;
    return nullptr;
}

// ---------------------------------------------------------------- sites

SiteSettings Library::Site(const std::string& host) const {
    const auto it = m_sites.find(host);
    return it != m_sites.end() ? it->second : SiteSettings{};
}

void Library::SetSite(const std::string& host, const SiteSettings& settings) {
    if (host.empty()) return;
    if (settings.IsDefault()) m_sites.erase(host);
    else m_sites[host] = settings;
}

void Library::ClearSite(const std::string& host) {
    m_sites.erase(host);
}

void Library::ClearSites() {
    m_sites.clear();
}

} // namespace UltraLight
