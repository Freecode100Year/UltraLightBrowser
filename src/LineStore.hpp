#pragma once

// The lines of "我的线路": share links the user imported or got from a
// subscription, and which one is in use. Plain data with no Windows calls, so it
// can be unit-tested; WarpManager keeps it DPAPI-encrypted in warp\lines.dat.

#include <cstdint>
#include <string>
#include <vector>

namespace UltraLight {

struct LineEntry {
    std::string id;
    std::string name;
    std::string link;
    std::string sub;  // id of the subscription it came from, or ""
};

struct SubEntry {
    std::string id;
    std::string name;
    std::string url;
    std::int64_t updated = 0;  // ms since the epoch of the last successful update
    int count = 0;             // lines it gave then
};

class LineStore {
public:
    static constexpr const char* kPublic = "public";  // the built-in public line
    static constexpr const char* kAuto = "auto";      // fastest of the first kAutoMax lines
    static constexpr size_t kMaxLines = 500;
    static constexpr size_t kAutoMax = 20;

    // false (and an empty store) when text is not a saved store.
    bool Load(const std::string& text);
    std::string Save() const;

    const std::vector<LineEntry>& Lines() const { return m_lines; }
    const std::vector<SubEntry>& Subs() const { return m_subs; }
    const std::string& Selected() const { return m_selected; }
    const LineEntry* Find(const std::string& id) const;

    // Adds the links that are not in the store yet; returns how many were added.
    size_t Add(const std::vector<std::string>& links, const std::string& sub = "");
    bool Remove(const std::string& id);
    bool Rename(const std::string& id, const std::string& name);
    bool Select(const std::string& id);  // kPublic, kAuto or a line id

    std::string AddSub(const std::string& name, const std::string& url);
    bool RemoveSub(const std::string& id);  // with its lines
    // Replaces the lines of a subscription. A selected line that is still in it
    // stays selected; one that is gone falls back to the public line.
    bool ReplaceSub(const std::string& id, const std::vector<std::string>& links, std::int64_t now);

    // The links to run: empty for the public line, one, or several for kAuto.
    std::vector<std::string> SelectedLinks() const;

    // Display helpers: "#name" of the link (VMess: its "ps"), "VLESS", "host:port".
    static std::string NameOf(const std::string& link);
    static std::string ProtocolOf(const std::string& link);
    static std::string ServerOf(const std::string& link);

private:
    std::string NextId(char prefix);
    std::vector<LineEntry> m_lines;
    std::vector<SubEntry> m_subs;
    std::string m_selected = kPublic;
    std::uint64_t m_nextId = 1;
};

} // namespace UltraLight
