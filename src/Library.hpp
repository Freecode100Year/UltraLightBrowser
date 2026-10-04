#pragma once

// User library: bookmarks, reading list, history, tab groups and per-site settings.
// Only bookmarks, the reading list and tab groups are written to disk; history and
// site settings exist for the running session only, so closing the browser leaves no
// record of visited sites. Plain C++ and UTF-8 strings only so it can be unit-tested
// off Windows; callers supply timestamps.

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

class Library {
public:
    explicit Library(std::filesystem::path directory);

    void Load();
    // Writes the library file when bookmarks, reading list or tab groups changed;
    // after a failed write the library stays dirty so the next save retries.
    bool Save();
    // Serializes changed stores and clears their dirty flags; the caller may write
    // the files on another thread with WriteFileAtomic.
    std::vector<std::pair<std::filesystem::path, std::string>> TakeSnapshot();
    static bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data);
    bool IsDirty() const { return m_dirtyLibrary; }
    void MarkDirty() { m_dirtyLibrary = true; }

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

    static std::string HostOf(const std::string& url);
    static bool IsWebUrl(const std::string& url);

private:
    std::string NewId(std::int64_t now);
    void PruneHistory();
    void LoadLibraryFile();

    std::filesystem::path m_dir;
    std::vector<std::string> m_folders;
    std::vector<Bookmark> m_bookmarks;
    std::vector<ReadingItem> m_reading;
    std::vector<TabGroup> m_groups;
    std::map<std::string, SiteSettings> m_sites;
    std::vector<HistoryEntry> m_history;
    std::map<std::string, std::size_t> m_historyIndex;
    std::uint64_t m_idCounter = 0;

    bool m_dirtyLibrary = false;
};

} // namespace UltraLight
