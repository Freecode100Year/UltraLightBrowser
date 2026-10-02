#pragma once

// User library: bookmarks, reading list, history, tab groups, per-site settings,
// privacy statistics and the saved session. Plain C++ and UTF-8 strings only so it
// can be unit-tested off Windows; callers supply timestamps and day keys.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace UltraLight {

inline constexpr const char* kFavoritesFolder = "个人收藏";
inline constexpr const char* kBookmarksFolder = "书签";

struct Bookmark {
    std::string id;
    std::string title;
    std::string url;
    std::string folder;
    std::int64_t added = 0;
};

struct ReadingItem {
    std::string id;
    std::string title;
    std::string url;
    std::int64_t added = 0;
    bool read = false;
};

struct HistoryEntry {
    std::string url;
    std::string title;
    std::int64_t last = 0;
    int visits = 0;
};

struct SavedTab {
    std::string title;
    std::string url;
};

struct TabGroup {
    std::string id;
    std::string name;
    std::vector<SavedTab> tabs;
    std::int64_t updated = 0;
};

struct SiteSettings {
    double zoom = 0.0;                 // 0 = browser default
    bool autoReader = false;
    std::string camera = "ask";        // ask / allow / deny
    std::string microphone = "ask";
    std::string location = "ask";
    std::string popups = "block";      // block / allow (script-opened windows)
    bool adblock = true;

    bool IsDefault() const;
};

struct Suggestion {
    std::string title;
    std::string url;
    bool bookmark = false;
};

struct PrivacyCount {
    std::string name;
    std::int64_t count = 0;
};

struct PrivacyReport {
    std::int64_t total = 0;
    std::vector<std::pair<std::string, std::int64_t>> perDay;  // oldest first
    std::vector<PrivacyCount> trackers;                         // most blocked first
    std::vector<PrivacyCount> sites;                            // sites with most blocked trackers
};

class Library {
public:
    explicit Library(std::filesystem::path directory);

    void Load();
    // Writes changed stores; history and privacy are batched, so call this from a
    // periodic timer and once more on exit.
    void Save();
    bool IsDirty() const { return m_dirtyLibrary || m_dirtyHistory || m_dirtyPrivacy || m_dirtySession; }

    // Bookmarks
    const std::vector<std::string>& Folders() const { return m_folders; }
    bool AddFolder(const std::string& name);
    bool RenameFolder(const std::string& from, const std::string& to);
    bool RemoveFolder(const std::string& name);  // bookmarks move to the default folder
    const std::vector<Bookmark>& Bookmarks() const { return m_bookmarks; }
    std::vector<Bookmark> BookmarksIn(const std::string& folder) const;
    std::string AddBookmark(const std::string& title, const std::string& url, const std::string& folder, std::int64_t now);
    bool UpdateBookmark(const std::string& id, const std::string& title, const std::string& url, const std::string& folder);
    bool RemoveBookmark(const std::string& id);
    bool MoveBookmark(const std::string& id, const std::string& beforeId);
    const Bookmark* FindBookmarkByUrl(const std::string& url) const;
    int ImportNetscapeHtml(const std::string& html, std::int64_t now);
    std::string ExportNetscapeHtml() const;

    // Reading list
    const std::vector<ReadingItem>& ReadingList() const { return m_reading; }
    std::string AddReading(const std::string& title, const std::string& url, std::int64_t now);
    bool RemoveReading(const std::string& id);
    bool SetRead(const std::string& id, bool read);
    void MarkReadByUrl(const std::string& url);

    // History (one entry per URL, most recent visit wins)
    void RecordVisit(const std::string& url, const std::string& title, std::int64_t now);
    void UpdateTitle(const std::string& url, const std::string& title);
    std::vector<HistoryEntry> QueryHistory(const std::string& query, std::size_t limit) const;
    std::vector<HistoryEntry> TopSites(std::size_t limit) const;
    bool RemoveHistory(const std::string& url);
    void ClearHistory(std::int64_t since);  // removes visits at or after `since` (0 = all)
    std::size_t HistorySize() const { return m_history.size(); }

    // Address bar suggestions from bookmarks and history
    std::vector<Suggestion> Suggest(const std::string& query, std::size_t limit) const;

    // Tab groups
    const std::vector<TabGroup>& Groups() const { return m_groups; }
    std::string SaveGroup(const std::string& name, const std::vector<SavedTab>& tabs, std::int64_t now);
    bool RenameGroup(const std::string& id, const std::string& name);
    bool RemoveGroup(const std::string& id);
    const TabGroup* FindGroup(const std::string& id) const;

    // Per-site settings, keyed by host
    SiteSettings Site(const std::string& host) const;
    void SetSite(const std::string& host, const SiteSettings& settings);
    void ClearSite(const std::string& host);
    void ClearSites();
    std::size_t SiteCount() const { return m_sites.size(); }

    // Privacy statistics (dayKey = local "YYYY-MM-DD")
    void RecordBlocked(const std::string& trackerDomain, const std::string& siteHost, const std::string& dayKey);
    PrivacyReport Report(const std::vector<std::string>& dayKeys, std::size_t topN) const;
    void ClearPrivacy();

    // Session restore (normal windows only)
    void SetSession(const std::vector<std::vector<SavedTab>>& windows);
    const std::vector<std::vector<SavedTab>>& Session() const { return m_session; }

    static std::string HostOf(const std::string& url);
    static bool IsWebUrl(const std::string& url);

private:
    std::string NewId(std::int64_t now);
    void PruneHistory();
    void PrunePrivacy();
    void LoadLibraryFile();
    void LoadHistoryFile();
    void LoadPrivacyFile();
    void LoadSessionFile();

    std::filesystem::path m_dir;
    std::vector<std::string> m_folders;
    std::vector<Bookmark> m_bookmarks;
    std::vector<ReadingItem> m_reading;
    std::vector<TabGroup> m_groups;
    std::map<std::string, SiteSettings> m_sites;
    std::vector<HistoryEntry> m_history;
    std::map<std::string, std::size_t> m_historyIndex;
    // day -> (tracker -> count), day -> (site -> count)
    std::map<std::string, std::map<std::string, std::int64_t>> m_trackersByDay;
    std::map<std::string, std::map<std::string, std::int64_t>> m_sitesByDay;
    std::vector<std::vector<SavedTab>> m_session;
    std::uint64_t m_idCounter = 0;

    bool m_dirtyLibrary = false;
    bool m_dirtyHistory = false;
    bool m_dirtyPrivacy = false;
    bool m_dirtySession = false;
};

} // namespace UltraLight
