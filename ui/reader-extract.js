/* Runs in the page (wrapped with Readability.js by the browser). Returns the article as JSON or null. */
try {
  const doc = document.cloneNode(true);
  const article = new Readability(doc, { charThreshold: 280 }).parse();
  if (!article || !article.content || (article.length || 0) < 140) return null;
  return JSON.stringify({ title: article.title, byline: article.byline, siteName: article.siteName,
    content: article.content, excerpt: article.excerpt, length: article.length, lang: article.lang, dir: article.dir,
    url: location.href });
} catch (e) {
  return null;
}
