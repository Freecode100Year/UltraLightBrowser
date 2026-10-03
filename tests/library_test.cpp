#include "Library.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>

using namespace UltraLight;

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "ulb-library-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "history.json") << R"([{"url":"https://old.example/","title":"x","last":1,"visits":1}])";
    std::ofstream(dir / "session.json") << "[]";
    {
        Library lib(dir);
        lib.Load();
        // Stores written by earlier versions are deleted, not imported.
        assert(lib.HistorySize() == 0 && !std::filesystem::exists(dir / "history.json") && !std::filesystem::exists(dir / "session.json"));
        assert(lib.Folders().size() == 2);
        const auto a = lib.AddBookmark("GitHub", "https://github.com/", kFavoritesFolder, 1000);
        const auto b = lib.AddBookmark("", "https://example.com/x", "missing", 1001);
        assert(!a.empty() && !b.empty());
        assert(lib.BookmarksIn(kFavoritesFolder).size() == 1);
        assert(lib.BookmarksIn(kBookmarksFolder).front().title == "https://example.com/x");
        assert(lib.AddFolder("新闻") && !lib.AddFolder("新闻") && !lib.RemoveFolder(kFavoritesFolder));
        assert(lib.UpdateBookmark(b, "Example", "", "新闻"));
        assert(lib.RenameFolder("新闻", "资讯") && lib.BookmarksIn("资讯").size() == 1);
        assert(lib.RemoveFolder("资讯") && lib.BookmarksIn(kBookmarksFolder).size() == 1);
        assert(lib.MoveBookmark(b, a) && lib.Bookmarks().front().id == b && lib.Bookmarks().front().folder == kFavoritesFolder);

        lib.RecordVisit("https://news.example.com/a", "A", 10);
        lib.RecordVisit("https://news.example.com/a", "A2", 20);
        lib.RecordVisit("https://news.example.com/b", "B", 30);
        lib.RecordVisit("https://other.org/", "Other", 40);
        lib.RecordVisit("javascript:alert(1)", "x", 50);
        lib.RecordVisit("https://ulb.internal/start.html", "x", 50);
        assert(lib.HistorySize() == 4);  // internal pages are https too; filtered by caller
        assert(lib.RemoveHistory("https://ulb.internal/start.html"));
        auto h = lib.QueryHistory("", 10);
        assert(h.size() == 3 && h.front().url == "https://other.org/");
        assert(lib.QueryHistory("a2", 10).size() == 1);
        lib.ClearHistory(35);
        assert(lib.HistorySize() == 2);

        auto sug = lib.Suggest("git", 5);
        assert(!sug.empty() && sug.front().url == "https://github.com/" && sug.front().bookmark);
        assert(lib.Suggest("news", 5).front().url.find("news.example.com") != std::string::npos);

        const auto r = lib.AddReading("Long read", "https://example.com/read", 5);
        lib.MarkReadByUrl("https://example.com/read");
        assert(lib.ReadingList().front().read);
        lib.AddReading("Again", "https://example.com/read", 6);
        assert(lib.ReadingList().size() == 1 && !lib.ReadingList().front().read);
        assert(lib.SetRead(r, true) && lib.RemoveReading(r) && lib.ReadingList().empty());

        const auto g = lib.SaveGroup("工作", {{"A", "https://a.com/"}, {"blank", "about:blank"}}, 7);
        assert(!g.empty() && lib.FindGroup(g)->tabs.size() == 1);
        assert(lib.RenameGroup(g, "研究"));

        SiteSettings s;
        s.zoom = 1.25;
        s.camera = "deny";
        lib.SetSite("example.com", s);
        assert(lib.Site("example.com").camera == "deny" && lib.Site("none.com").IsDefault());
        lib.SetSite("default.com", SiteSettings{});
        assert(lib.SiteCount() == 1);

        lib.Save();
        assert(!lib.IsDirty());
    }
    {
        Library lib(dir);
        lib.Load();
        // History and site settings never reach the disk.
        assert(lib.Bookmarks().size() == 2 && lib.HistorySize() == 0 && lib.Groups().size() == 1);
        assert(lib.Site("example.com").IsDefault());
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            assert(entry.path().filename() == "library.json" || entry.is_directory());
        }
        std::ifstream in(dir / "library.json");
        const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(saved.find("news.example.com") == std::string::npos && saved.find("example.com\"") == std::string::npos);

        const std::string html = R"html(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<DL><p>
<DT><H3 PERSONAL_TOOLBAR_FOLDER="true">Favorites bar</H3>
<DL><p>
<DT><A HREF="https://bar.example/?a=1&amp;b=2" ADD_DATE="1">Bar &amp; Grill</A>
<DT><H3>Nested</H3>
<DL><p><DT><A href='https://nested.example/'>Nested link</A></DL><p>
</DL><p>
<DT><A HREF="https://top.example/">Top</A>
<DT><A HREF="javascript:evil()">bad</A>
</DL>)html";
        assert(lib.ImportNetscapeHtml(html, 9) == 3);
        assert(lib.ImportNetscapeHtml(html, 9) == 0);
        bool found = false;
        for (const auto& b : lib.Bookmarks()) {
            if (b.url == "https://bar.example/?a=1&b=2") { found = b.title == "Bar & Grill" && b.folder == kFavoritesFolder; }
        }
        assert(found);
        assert(!lib.BookmarksIn("Nested").empty());
        const auto out = lib.ExportNetscapeHtml();
        assert(out.find("Bar &amp; Grill") != std::string::npos && out.find("PERSONAL_TOOLBAR_FOLDER") != std::string::npos);
        Library other(dir / "copy");
        assert(other.ImportNetscapeHtml(out, 1) == static_cast<int>(lib.Bookmarks().size()));
    }
    assert(Library::HostOf("https://user:pw@Sub.Example.COM:8080/x?y") == "sub.example.com");
    assert(Library::HostOf("http://[::1]:80/") == "[::1]");
    std::filesystem::remove_all(dir);
    std::cout << "Library tests passed\n";
}
