"use strict";
const { el, icon, call, host, modal, toast, normalizeUrl } = ULB;
let data = { folders: [], bookmarks: [] };
let current = "个人收藏";
let dragId = null;
const q = document.getElementById("q");

function renderFolders() {
  const fixed = new Set(["个人收藏", "书签"]);
  document.getElementById("folders").replaceChildren(...data.folders.map((f) => {
    const count = data.bookmarks.filter((b) => b.folder === f).length;
    const row = el("div", { class: "folder" + (f === current && !q.value ? " on" : ""), onclick: () => { current = f; q.value = ""; render(); } },
      el("span", { text: f === "个人收藏" ? "★" : "▸", class: "faint" }), el("span", { text: f }),
      el("span", { class: "n", text: String(count) }));
    if (!fixed.has(f)) {
      row.append(el("button", { class: "icon fx", title: "重命名", onclick: async (e) => {
        e.stopPropagation();
        const r = await modal("重命名文件夹", [{ name: "name", label: "名称", value: f, required: true }]);
        if (r && r.name) { await call("renameFolder", { from: f, to: r.name }); if (current === f) current = r.name; load(); }
      } }, "✎"), el("button", { class: "icon fx", title: "删除文件夹（书签移到“书签”）", onclick: async (e) => {
        e.stopPropagation();
        if (!confirm(`删除文件夹“${f}”？其中的书签会移到“书签”。`)) return;
        await call("removeFolder", { name: f }); if (current === f) current = "书签"; load();
      } }, "✕"));
    }
    row.addEventListener("dragover", (e) => { if (dragId) e.preventDefault(); });
    row.addEventListener("drop", async (e) => {
      e.preventDefault();
      const b = data.bookmarks.find((x) => x.id === dragId);
      if (b && b.folder !== f) { await call("updateBookmark", { id: b.id, title: b.title, url: b.url, folder: f }); load(); }
    });
    return row;
  }));
}

function renderList() {
  const term = q.value.trim().toLowerCase();
  const items = term
    ? data.bookmarks.filter((b) => (b.title + " " + b.url).toLowerCase().includes(term))
    : data.bookmarks.filter((b) => b.folder === current);
  const list = document.getElementById("list");
  if (!items.length) { list.replaceChildren(el("div", { class: "empty", text: term ? "没有匹配的书签" : "此文件夹为空" })); return; }
  list.replaceChildren(...items.map((b) => {
    const row = el("a", { class: "item", href: b.url, draggable: "true" },
      icon(b.url, 16),
      el("div", { class: "t" }, el("div", { text: b.title }), el("div", { class: "url", text: term ? b.folder + " · " + host(b.url) : b.url })),
      el("button", { class: "icon x", title: "编辑", onclick: async (e) => {
        e.preventDefault(); e.stopPropagation();
        const r = await modal("编辑书签", [
          { name: "title", label: "名称", value: b.title },
          { name: "url", label: "网址", value: b.url, required: true },
          { name: "folder", label: "文件夹", value: b.folder, options: data.folders }]);
        if (r) { await call("updateBookmark", { id: b.id, title: r.title, url: normalizeUrl(r.url), folder: r.folder }); load(); }
      } }, "✎"),
      el("button", { class: "icon x", title: "删除", onclick: async (e) => {
        e.preventDefault(); e.stopPropagation();
        await call("removeBookmark", { id: b.id }); load();
      } }, "✕"));
    row.addEventListener("dragstart", (e) => { dragId = b.id; e.dataTransfer.effectAllowed = "move"; });
    row.addEventListener("dragend", () => { dragId = null; });
    row.addEventListener("dragover", (e) => { if (dragId && dragId !== b.id) { e.preventDefault(); row.classList.add("drop"); } });
    row.addEventListener("dragleave", () => row.classList.remove("drop"));
    row.addEventListener("drop", async (e) => {
      e.preventDefault(); row.classList.remove("drop");
      if (dragId && dragId !== b.id) { await call("moveBookmark", { id: dragId, before: b.id }); load(); }
    });
    return row;
  }));
}

function render() { renderFolders(); renderList(); }

async function load() {
  data = await call("getBookmarks");
  if (!data.folders.includes(current)) current = data.folders[0] || "书签";
  render();
}

q.addEventListener("input", render);
document.getElementById("new-folder").onclick = async () => {
  const r = await modal("新建文件夹", [{ name: "name", label: "名称", required: true }]);
  if (r && r.name) { await call("addFolder", { name: r.name }); current = r.name; load(); }
};
document.getElementById("add").onclick = async () => {
  const r = await modal("添加书签", [
    { name: "title", label: "名称" },
    { name: "url", label: "网址", required: true, placeholder: "https://" },
    { name: "folder", label: "文件夹", value: current, options: data.folders }]);
  if (r && r.url) { await call("addBookmark", { title: r.title, url: normalizeUrl(r.url), folder: r.folder }); load(); }
};
document.getElementById("import").onclick = async () => {
  const r = await call("importBookmarks");
  if (r && r.count >= 0) { toast(`已导入 ${r.count} 个书签`); load(); }
};
document.getElementById("export").onclick = async () => {
  const r = await call("exportBookmarks");
  if (r && r.saved) toast("已导出书签");
};
if (location.hash === "#reading") current = "书签";
ULB.on("library", load);
load();
