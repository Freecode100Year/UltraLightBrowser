/* In-page find. Invoked as (FIND)(action, query); returns {count, index}. Uses the CSS Custom Highlight API
   so the page DOM is not modified apart from one style element while results are shown. */
(action, query) => {
  const KEY = Symbol.for("ulb.find");
  let st = window[KEY];
  if (!st) {
    st = { q: "", ranges: [], i: -1, style: null };
    Object.defineProperty(window, KEY, { value: st });
  }
  const hl = typeof CSS !== "undefined" && CSS.highlights;
  const clear = () => {
    if (hl) { CSS.highlights.delete("ulb-find"); CSS.highlights.delete("ulb-find-cur"); }
    if (st.style) { st.style.remove(); st.style = null; }
    st.ranges = []; st.i = -1; st.q = "";
  };
  if (action === "clear" || !query) { clear(); return { count: 0, index: 0 }; }
  if (query !== st.q || action === "refresh") {
    clear();
    st.q = query;
    const needle = query.toLocaleLowerCase();
    const walker = document.createTreeWalker(document.body || document.documentElement, NodeFilter.SHOW_TEXT, {
      acceptNode(n) {
        const p = n.parentElement;
        if (!p || /^(SCRIPT|STYLE|NOSCRIPT|TEMPLATE|TEXTAREA)$/.test(p.tagName)) return NodeFilter.FILTER_REJECT;
        return NodeFilter.FILTER_ACCEPT;
      } });
    const visible = new Map();
    for (let n = walker.nextNode(); n && st.ranges.length < 5000; n = walker.nextNode()) {
      const text = n.nodeValue.toLocaleLowerCase();
      if (text.length !== n.nodeValue.length) continue;
      let at = text.indexOf(needle);
      if (at < 0) continue;
      const p = n.parentElement;
      if (!visible.has(p)) visible.set(p, p.checkVisibility ? p.checkVisibility({ visibilityProperty: true }) : !!p.getClientRects().length);
      if (!visible.get(p)) continue;
      while (at >= 0) {
        const r = new Range();
        r.setStart(n, at); r.setEnd(n, at + needle.length);
        st.ranges.push(r);
        at = text.indexOf(needle, at + needle.length);
      }
    }
    if (hl && st.ranges.length) {
      st.style = document.createElement("style");
      st.style.textContent = "::highlight(ulb-find){background:#ffd60a66;color:inherit}::highlight(ulb-find-cur){background:#ffd60a;color:#000}";
      (document.head || document.documentElement).append(st.style);
      CSS.highlights.set("ulb-find", new Highlight(...st.ranges));
    }
    // Start from the first match below the current scroll position.
    st.i = st.ranges.findIndex((r) => r.getBoundingClientRect().bottom >= 0);
    if (st.i < 0) st.i = 0;
    if (action === "prev") st.i = (st.i - 1 + st.ranges.length) % Math.max(1, st.ranges.length);
  } else if (st.ranges.length) {
    st.i = action === "prev" ? (st.i - 1 + st.ranges.length) % st.ranges.length : (st.i + 1) % st.ranges.length;
  }
  if (!st.ranges.length) return { count: 0, index: 0 };
  const cur = st.ranges[st.i];
  if (hl) CSS.highlights.set("ulb-find-cur", new Highlight(cur));
  const el = cur.startContainer.parentElement;
  if (el) {
    el.scrollIntoView({ block: "center", inline: "nearest", behavior: "instant" });
    const rect = cur.getBoundingClientRect();
    if (rect.top < 0 || rect.bottom > innerHeight) window.scrollBy(0, rect.top - innerHeight / 2);
  }
  return { count: st.ranges.length, index: st.i + 1 };
}
