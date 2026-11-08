#include "AdBlockRules.hpp"

#include <algorithm>
#include <cwctype>

namespace UltraLight::AdBlock {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// host == domain or host is a subdomain of domain.
bool IsUnder(const std::wstring& host, const std::wstring& domain) {
    if (host.size() == domain.size()) return host == domain;
    if (host.size() < domain.size() + 1) return false;
    return host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == L'.';
}

bool IsWebScheme(const std::wstring& uri) {
    const auto end = uri.find(L"://");
    if (end == std::wstring::npos || end > 5) return false;
    const std::wstring scheme = Lower(uri.substr(0, end));
    return scheme == L"https" || scheme == L"http" || scheme == L"wss" || scheme == L"ws";
}

// Lowercase path + query of an absolute URL ("/" when there is none). The
// fragment is never sent to the network and is ignored.
std::wstring LowerPath(const std::wstring& uri) {
    const auto proto = uri.find(L"://");
    if (proto == std::wstring::npos) return L"/";
    const auto start = uri.find_first_of(L"/?#", proto + 3);
    if (start == std::wstring::npos || uri[start] == L'#') return L"/";
    const auto hash = uri.find(L'#', start);
    std::wstring path = uri.substr(start, hash == std::wstring::npos ? std::wstring::npos : hash - start);
    if (path[0] == L'?') path.insert(path.begin(), L'/');
    return Lower(std::move(path));
}

const std::unordered_set<std::wstring> kMultiTenantSuffixes = {
    L"github.io", L"gitlab.io", L"gitee.io",
    L"blogspot.com", L"wordpress.com", L"appspot.com",
    L"pages.dev", L"workers.dev", L"r2.dev", L"trycloudflare.com",
    L"vercel.app", L"netlify.app", L"web.app", L"firebaseapp.com",
    L"herokuapp.com", L"azurewebsites.net", L"azureedge.net", L"cloudfront.net",
    L"fastly.net", L"fly.dev", L"render.com", L"onrender.com",
    L"myshopify.com", L"railway.app", L"glitch.me", L"surge.sh"
};

bool IsPublicSuffix(const std::wstring& sld, const std::wstring& tld) {
    if (tld.length() == 2) {
        if (sld == L"com" || sld == L"net" || sld == L"org" || sld == L"gov" ||
            sld == L"gob" || sld == L"edu" || sld == L"co" || sld == L"ne" ||
            sld == L"ac" || sld == L"go" || sld == L"or" || sld == L"re") {
            return true;
        }
    }
    return kMultiTenantSuffixes.count(sld + L"." + tld) != 0;
}

// ------------------------------------------------------------------ surrogates
// Minimal stand-ins for blocked libraries. Sites often wait for an analytics
// callback before following a link or submitting a form, or crash when
// googletag/adsbygoogle is missing; these keep such pages working while nothing
// is sent anywhere.

const char kSurrogateAnalytics[] = R"ULBJS((function(){'use strict';
var noop=function(){};
function Tracker(){}
Tracker.prototype.get=function(){};Tracker.prototype.set=noop;Tracker.prototype.send=function(){fire(arguments);};
function fire(args){for(var i=0;i<args.length;i++){var o=args[i];if(o&&typeof o==='object'&&typeof o.hitCallback==='function'){try{o.hitCallback();}catch(e){}}}}
var ga=function(){var a=arguments;if(!a.length)return;if(typeof a[0]==='function'){try{a[0](new Tracker());}catch(e){}return;}fire(a);};
ga.create=function(){return new Tracker();};ga.getByName=function(){return new Tracker();};
ga.getAll=function(){return [];};ga.remove=noop;ga.loaded=true;ga.l=Date.now();
var name=window.GoogleAnalyticsObject||'ga';var old=window[name];window[name]=ga;
var q=old&&old.q;if(q&&q.length){for(var i=0;i<q.length;i++){try{ga.apply(null,q[i]);}catch(e){}}}
})();)ULBJS";

const char kSurrogateGtag[] = R"ULBJS((function(){'use strict';
var dl=window.dataLayer=window.dataLayer||[];
function later(f){if(typeof f==='function'){setTimeout(function(){try{f();}catch(e){}},1);}}
function run(o){if(!o||typeof o!=='object')return;later(o.eventCallback);
if(o.length>=3&&o[0]==='event'&&o[2]&&typeof o[2]==='object')later(o[2].event_callback);}
for(var i=0;i<dl.length;i++)run(dl[i]);
var push=dl.push;dl.push=function(){for(var i=0;i<arguments.length;i++)run(arguments[i]);return push.apply(this,arguments);};
if(typeof window.gtag!=='function'){window.gtag=function(){dl.push(arguments);};}
window.google_tag_manager=window.google_tag_manager||{};
})();)ULBJS";

const char kSurrogateGpt[] = R"ULBJS((function(){'use strict';
var noop=function(){};
var g=window.googletag=window.googletag||{};var queued=g.cmd;
function chain(o,names){names.forEach(function(n){o[n]=function(){return o;};});return o;}
function Slot(path,div){this._p=path||'';this._d=div||'';}
var sp=Slot.prototype;
['addService','clearCategoryExclusions','clearTargeting','defineSizeMapping','setCategoryExclusion','setClickUrl','setCollapseEmptyDiv','setConfig','setForceSafeFrame','setSafeFrameConfig','setTargeting','updateTargetingFromMap'].forEach(function(n){sp[n]=function(){return this;};});
sp.getAdUnitPath=function(){return this._p;};sp.getSlotElementId=function(){return this._d;};
sp.getSlotId=function(){var s=this;return {getId:function(){return s._p;},getDomId:function(){return s._d;}};};
sp.getTargeting=function(){return [];};sp.getTargetingKeys=function(){return [];};sp.getSizes=function(){return [];};
sp.getResponseInformation=function(){return null;};sp.getCategoryExclusions=function(){return [];};
var pubads=chain({},['addEventListener','removeEventListener','clearCategoryExclusions','clearTargeting','collapseEmptyDivs','disableInitialLoad','enableAsyncRendering','enableLazyLoad','enableSingleRequest','enableVideoAds','set','setCategoryExclusion','setCentering','setForceSafeFrame','setLocation','setPrivacySettings','setPublisherProvidedId','setRequestNonPersonalizedAds','setSafeFrameConfig','setTargeting','setVideoContent','updateCorrelator']);
pubads.refresh=noop;pubads.clear=function(){return true;};pubads.get=function(){return null;};
pubads.getSlots=function(){return [];};pubads.getTargeting=function(){return [];};pubads.getTargetingKeys=function(){return [];};
pubads.isInitialLoadDisabled=function(){return false;};pubads.display=noop;
var companion=chain({},['addEventListener','removeEventListener','setRefreshUnfilledSlots']);
var content=chain({},['addEventListener','removeEventListener','setContent']);
g.pubads=function(){return pubads;};g.companionAds=function(){return companion;};g.content=function(){return content;};
g.defineSlot=function(p,s,d){return new Slot(p,d);};
g.defineOutOfPageSlot=function(p,d){return new Slot(p,typeof d==='string'?d:'');};
g.destroySlots=function(){return true;};g.display=noop;g.enableServices=noop;g.disablePublisherConsole=noop;
g.openConsole=noop;g.setAdIframeTitle=noop;g.setConfig=noop;g.getVersion=function(){return '';};
g.sizeMapping=function(){var b={addSize:function(){return b;},build:function(){return [];}};return b;};
g.enums={OutOfPageFormat:{}};g.apiReady=true;g.pubadsReady=true;
g.cmd={push:function(){for(var i=0;i<arguments.length;i++){try{arguments[i].call(window);}catch(e){}}return 0;}};
if(queued&&queued.length){for(var i=0;i<queued.length;i++){try{queued[i].call(window);}catch(e){}}}
})();)ULBJS";

const char kSurrogateAdsByGoogle[] = R"ULBJS((function(){'use strict';
window.adsbygoogle={loaded:true,push:function(){}};
})();)ULBJS";

const char* SurrogateFor(const std::wstring& host, const std::wstring& path) {
    if (IsUnder(host, L"google-analytics.com") && StartsWith(path, L"/analytics.js")) return kSurrogateAnalytics;
    if (IsUnder(host, L"googletagmanager.com") && (StartsWith(path, L"/gtag/js") || StartsWith(path, L"/gtm.js"))) return kSurrogateGtag;
    if ((IsUnder(host, L"googletagservices.com") || IsUnder(host, L"doubleclick.net")) && StartsWith(path, L"/tag/js/gpt.js")) return kSurrogateGpt;
    if (IsUnder(host, L"googlesyndication.com") && StartsWith(path, L"/pagead/js/adsbygoogle.js")) return kSurrogateAdsByGoogle;
    return nullptr;
}

// ------------------------------------------------------------ cosmetic script

// Element-hiding rules. Each selector becomes its own CSS rule, so one selector a
// future engine rejects cannot void the rest. Generic selectors only name things
// that are ads by construction (ad-network markup and ad iframes); vague class
// names such as ".ad" or ".banner" are left out because they hide real content.
const char kCosmeticScript[] = R"ULBJS((function(){'use strict';
if(window.__ulb_cosmetic)return;window.__ulb_cosmetic=true;
var CFG=__ULB_CONFIG__;
if(!CFG.enabled)return;
var host=location.hostname||'';
if(host==='ulb.internal')return;
function topHost(){
  try{if(window.top===window)return host;}catch(e){}
  var ao=location.ancestorOrigins;
  if(ao&&ao.length){try{return new URL(ao[ao.length-1]).hostname;}catch(e){}}
  try{return window.top.location.hostname;}catch(e){}
  return host;
}
if(CFG.off.indexOf(topHost())>=0)return;
function on(d){return host===d||(host.length>d.length&&host.slice(-d.length-1)==='.'+d);}
var GENERIC=[
'ins.adsbygoogle','[data-ad-client]','[data-ad-slot]','[id^="google_ads_iframe"]','[id^="div-gpt-ad"]',
'[id^="google_ads_frame"]','[data-google-query-id]','amp-ad','amp-embed[type="taboola"]','amp-embed[type="outbrain"]',
'iframe[src*="doubleclick.net/"]','iframe[src*="googlesyndication.com/"]','iframe[src*="amazon-adsystem.com/"]',
'iframe[src*="adnxs.com/"]','iframe[src*="taboola.com/"]','iframe[src*="outbrain.com/"]','iframe[src*="mgid.com/"]',
'iframe[src*="pos.baidu.com/"]','iframe[src*="cpro.baidu.com/"]','iframe[src*="gdt.qq.com/"]','iframe[src*="tanx.com/"]',
'a[href^="https://ad.doubleclick.net/"]','a[href*=".g.doubleclick.net/"]','a[href^="https://www.googleadservices.com/pagead/aclk"]',
'[id^="taboola-"]','.trc_related_container','.trc_rbox_container','.OUTBRAIN','.ob-widget','.ob-smartfeed-wrapper',
'.mgbox','[id^="ScriptRoot"][data-type="mgid"]','div[id^="BAIDU_SSP"]','div[id^="cpro_u"]','[id^="baidu_dup_"]',
'#carbonads','.carbonads','#bsa-zone','div[id^="bsa-zone_"]','.adthrive-ad','.adsbyvli','[class^="adngin-"]'
];
var SITES={
'youtube.com':['#masthead-ad','ytd-ad-slot-renderer','ytd-in-feed-ad-layout-renderer','ytd-banner-promo-renderer',
  'ytd-statement-banner-renderer','ytd-promoted-sparkles-web-renderer','ytd-promoted-video-renderer',
  'ytd-display-ad-renderer','ytd-compact-promoted-video-renderer','ytd-action-companion-ad-renderer',
  'ytd-player-legacy-desktop-watch-ads-renderer','ytd-engagement-panel-section-list-renderer[target-id="engagement-panel-ads"]',
  '#player-ads','.ytp-ad-overlay-container','.ytp-ad-image-overlay','.ytp-featured-product',
  'ytd-rich-item-renderer:has(> #content > ytd-ad-slot-renderer)','ytd-rich-section-renderer:has(ytd-statement-banner-renderer)',
  'ytd-reel-video-renderer:has(ytd-ad-slot-renderer)','ytm-promoted-sparkles-web-renderer','ad-slot-renderer'],
'baidu.com':['#content_right > div:has(a[href*="baidu.com/baidu.php"])','#content_left > div:has(a[href*="baidu.com/baidu.php"])',
  '#content_left > div[cmatchid]','.ec_wise_ad','.ec-tuiguang'],
'zhihu.com':['.Pc-card.Card','.Pc-word','.Banner-link','.TopstoryItem--advertCard','.AdvertImg'],
'weibo.com':['[class*="wbpro-ad"]','[class*="_wbad_"]'],
'csdn.net':['.toolbar-advert','#recommendAdBox','.recommend-right_aside .kind_person+.box-shadow','[id^="kp_box_"]','.passport-login-tip-container'],
'bilibili.com':['.ad-report','.video-card-ad-small','.bili-video-card:has(.bili-video-card__info--ad)','.feed-card:has(.bili-video-card__info--ad)','.slide-ad-exp','.activity-m-v1'],
'sohu.com':['[data-role="god_column"]','.godR','.god-mod'],
'163.com':['.post_side_ad','.ad_hot_top','.N-nav-channel-ad','[class*="ne_ad_"]'],
'douban.com':['[id^="dale_"]'],
'sina.com.cn':['.sinaads','ins.sinaads','[id^="ad_"][class*="sina"]']
};
var list=GENERIC.slice();
for(var d in SITES){if(on(d))list=list.concat(SITES[d]);}
function addStyle(){
  var root=document.head||document.documentElement;if(!root)return false;
  var st=document.createElement('style');st.id='__ulb_cosmetic_css__';
  st.textContent=list.map(function(s){return s+'{display:none!important}';}).join('\n');
  root.appendChild(st);return true;
}
if(!addStyle())document.addEventListener('DOMContentLoaded',addStyle,{once:true});
if(on('youtube.com'))(function(){
  var KEYS=['adPlacements','adSlots','playerAds','adBreakHeartbeatParams'];
  function prune(o){
    if(!o||typeof o!=='object')return o;
    try{
      for(var i=0;i<KEYS.length;i++){if(Object.prototype.hasOwnProperty.call(o,KEYS[i]))delete o[KEYS[i]];}
      if(o.playerResponse&&typeof o.playerResponse==='object')prune(o.playerResponse);
      if(Array.isArray(o)){for(var j=0;j<o.length;j++){if(o[j]&&o[j].playerResponse)prune(o[j].playerResponse);}}
    }catch(e){}
    return o;
  }
  var parse=JSON.parse;
  var wrapped=function(){return prune(parse.apply(this,arguments));};
  try{Object.defineProperty(wrapped,'name',{value:'parse'});wrapped.toString=function(){return parse.toString();};}catch(e){}
  JSON.parse=wrapped;
  if(window.Response&&Response.prototype.json){
    var rj=Response.prototype.json;
    Response.prototype.json=function(){return rj.apply(this,arguments).then(prune);};
  }
  ['ytInitialPlayerResponse','playerResponse'].forEach(function(name){
    var value;
    try{Object.defineProperty(window,name,{configurable:true,enumerable:true,
      get:function(){return value;},set:function(v){value=prune(v);}});}catch(e){}
  });
  if(window.top!==window)return;
  // Fallback for ads that still arrive: end them at once and press "Skip".
  setInterval(function(){
    var p=document.querySelector('.html5-video-player.ad-showing');
    if(!p)return;
    var v=p.querySelector('video.html5-main-video')||p.querySelector('video');
    if(v&&isFinite(v.duration)&&v.duration>0&&v.currentTime<v.duration-0.1){try{v.currentTime=v.duration;}catch(e){}}
    var b=p.querySelector('.ytp-skip-ad-button,.ytp-ad-skip-button,.ytp-ad-skip-button-modern,.ytp-ad-skip-button-slot button');
    if(b)b.click();
  },300);
})();
})();)ULBJS";

std::wstring Widen(const char* s) {
    std::wstring out;
    for (; *s; ++s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return out;
}

std::wstring JsonString(const std::wstring& s) {
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"' || ch == L'\\') {
            out.push_back(L'\\');
            out.push_back(ch);
        } else if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == 0x2028 || ch == 0x2029) {
            const unsigned v = static_cast<unsigned>(ch);
            out += L"\\u";
            out.push_back(hex[(v >> 12) & 0xF]);
            out.push_back(hex[(v >> 8) & 0xF]);
            out.push_back(hex[(v >> 4) & 0xF]);
            out.push_back(hex[v & 0xF]);
        } else {
            out.push_back(ch);
        }
    }
    out.push_back(L'"');
    return out;
}

} // namespace

std::wstring ExtractHost(const std::wstring& uri) {
    const size_t protoEnd = uri.find(L"://");
    if (protoEnd == std::wstring::npos) return L"";

    const size_t authStart = protoEnd + 3;
    size_t authEnd = uri.find_first_of(L"/?#", authStart);
    if (authEnd == std::wstring::npos) authEnd = uri.length();
    if (authStart >= authEnd) return L"";

    size_t hostStart = authStart;
    const size_t atPos = uri.rfind(L'@', authEnd - 1);
    if (atPos != std::wstring::npos && atPos >= authStart) hostStart = atPos + 1;
    if (hostStart >= authEnd) return L"";

    std::wstring host;
    if (uri[hostStart] == L'[') {
        const size_t closeBracket = uri.find(L']', hostStart);
        host = (closeBracket != std::wstring::npos && closeBracket < authEnd)
            ? uri.substr(hostStart, closeBracket - hostStart + 1)
            : uri.substr(hostStart, authEnd - hostStart);
    } else {
        const size_t colonPos = uri.find(L':', hostStart);
        const size_t hostEnd = (colonPos != std::wstring::npos && colonPos < authEnd) ? colonPos : authEnd;
        host = uri.substr(hostStart, hostEnd - hostStart);
    }
    while (!host.empty() && host.back() == L'.') host.pop_back();
    return Lower(std::move(host));
}

std::wstring BaseDomain(const std::wstring& host) {
    if (host.empty()) return L"";
    if (host.front() == L'[') return host;  // IPv6 literal

    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= host.length()) {
        const size_t dot = host.find(L'.', start);
        if (dot == std::wstring::npos) { parts.push_back(host.substr(start)); break; }
        parts.push_back(host.substr(start, dot - start));
        start = dot + 1;
    }
    if (parts.size() <= 2) return host;

    // IPv4 literal: the whole address is the "domain".
    if (std::all_of(host.begin(), host.end(), [](wchar_t c) { return c == L'.' || (c >= L'0' && c <= L'9'); })) {
        return host;
    }

    const auto& tld = parts[parts.size() - 1];
    const auto& sld = parts[parts.size() - 2];
    const size_t takeParts = IsPublicSuffix(sld, tld) ? 3 : 2;
    if (parts.size() < takeParts) return host;

    std::wstring base;
    for (size_t i = parts.size() - takeParts; i < parts.size(); ++i) {
        if (!base.empty()) base += L".";
        base += parts[i];
    }
    return base;
}

bool IsThirdParty(const std::wstring& reqHost, const std::wstring& topHost) {
    if (reqHost.empty() || topHost.empty()) return true;
    if (reqHost == topHost) return false;
    return BaseDomain(reqHost) != BaseDomain(topHost);
}

const Rules& Rules::Default() {
    static const Rules s_rules;
    return s_rules;
}

Rules::Rules() {
    // Hosts that serve only ads, trackers or telemetry. Subdomains match too.
    // Sentry.io and Bugsnag.com stay out (error reporting that sites depend on).
    m_domains = {
        // Google ads & analytics
        L"doubleclick.net", L"google-analytics.com", L"googletagmanager.com",
        L"googlesyndication.com", L"adservice.google.com", L"partner.googleadservices.com",
        L"googletagservices.com", L"googleadservices.com", L"2mdn.net", L"googleoptimize.com",
        L"app-measurement.com",
        // Baidu
        L"pos.baidu.com", L"cpro.baidu.com", L"hm.baidu.com", L"dup.baidustatic.com", L"cm.baidu.com",
        L"sp1.baidu.com", L"eclick.baidu.com", L"nsclick.baidu.com", L"mobads.baidu.com",
        L"mobads-logs.baidu.com", L"hmcdn.baidu.com", L"cbjs.baidu.com", L"drmcmm.baidu.com",
        L"cpro.baidustatic.com", L"ubmcmm.baidustatic.com",
        // Tencent / WeChat
        L"e.qq.com", L"gdt.qq.com", L"pgdt.gtimg.cn", L"tajs.qq.com", L"tmead.qq.com",
        L"pingjs.qq.com", L"pingtcss.qq.com", L"btrace.qq.com", L"adsmind.gdtimg.com",
        // Alibaba
        L"alimama.com", L"tanx.com", L"mmstat.com", L"atanx.alicdn.com",
        // ByteDance / Bilibili / NetEase / Sina / misc. China
        L"log.byteoversea.com", L"pangolin-sdk-toutiao.com", L"pglstatp-toutiao.com",
        L"cm.bilibili.com", L"analytics.163.com", L"sax.sina.com.cn", L"beacon.sina.com.cn",
        L"cnzz.com", L"cnzz.net", L"umeng.com", L"umengcloud.com", L"51.la", L"miaozhen.com",
        L"growingio.com", L"sensorsdata.cn", L"irs01.com", L"irs01.net", L"admaster.com.cn",
        L"mediav.com", L"ipinyou.com", L"adsame.com", L"allyes.com", L"linezing.com",
        L"gridsumdissector.com", L"zhugeio.com", L"talkingdata.com", L"vamaker.com", L"wrating.com",
        // Social network pixels
        L"connect.facebook.net", L"ads-twitter.com", L"analytics.twitter.com", L"ads.tiktok.com",
        L"analytics.tiktok.com", L"ads.linkedin.com", L"snap.licdn.com", L"ct.pinterest.com",
        L"tr.snapchat.com", L"sc-static.net", L"alb.reddit.com", L"events.redditmedia.com",
        // Ad exchanges, SSPs, DSPs and ad servers
        L"adnxs.com", L"adnxs-simple.com", L"criteo.com", L"criteo.net", L"amazon-adsystem.com",
        L"assoc-amazon.com", L"rubiconproject.com", L"casalemedia.com", L"pubmatic.com", L"openx.net",
        L"smartadserver.com", L"adroll.com", L"yieldmo.com", L"adsrvr.org", L"advertising.com",
        L"adform.net", L"adition.com", L"adtech.de", L"bidswitch.net", L"contextweb.com",
        L"indexww.com", L"lijit.com", L"sovrn.com", L"sharethrough.com", L"triplelift.com",
        L"3lift.com", L"teads.tv", L"33across.com", L"gumgum.com", L"media.net", L"yieldlab.net",
        L"adkernel.com", L"emxdgt.com", L"spotxchange.com", L"spotx.tv", L"springserve.com",
        L"smaato.net", L"inmobi.com", L"applovin.com", L"onetag-sys.com", L"richaudience.com",
        L"improvedigital.com", L"360yield.com", L"serving-sys.com", L"flashtalking.com",
        L"mathtag.com", L"rfihub.com", L"adgrx.com", L"simpli.fi", L"steelhousemedia.com",
        L"dotomi.com", L"ipredictive.com", L"owneriq.net", L"zemanta.com", L"adsco.re",
        L"adhigh.net", L"yieldoptimizer.com", L"ads.yahoo.com", L"analytics.yahoo.com",
        L"stickyadstv.com", L"tremorhub.com", L"smartclip.net", L"atdmt.com", L"mookie1.com",
        L"turn.com", L"bidr.io", L"adzerk.net", L"buysellads.com", L"carbonads.net",
        L"bounceexchange.com", L"bouncex.net", L"adpushup.com",
        // Native / recommendation ads
        L"outbrain.com", L"outbrainimg.com", L"taboola.com", L"revcontent.com", L"mgid.com",
        L"zergnet.com", L"nativo.com", L"adblade.com", L"content.ad", L"contentad.net",
        // Ad verification
        L"adsafeprotected.com", L"moatads.com", L"doubleverify.com", L"iasds01.com",
        // Measurement, data brokers, identity sync, session recording
        L"scorecardresearch.com", L"statcounter.com", L"hotjar.com", L"hotjar.io", L"clarity.ms",
        L"segment.io", L"mixpanel.com", L"amplitude.com", L"branch.io", L"adjust.com",
        L"appsflyer.com", L"quantserve.com", L"quantcount.com", L"chartbeat.com", L"chartbeat.net",
        L"nr-data.net", L"js-agent.newrelic.com", L"bat.bing.com", L"krxd.net", L"bluekai.com",
        L"exelator.com", L"demdex.net", L"omtrdc.net", L"everesttech.net", L"agkn.com",
        L"tapad.com", L"crwdcntrl.net", L"rlcdn.com", L"adsymptotic.com", L"liadm.com",
        L"id5-sync.com", L"pippio.com", L"eyeota.net", L"lotame.com", L"zeotap.com",
        L"permutive.com", L"permutive.app", L"parsely.com", L"fullstory.com", L"mouseflow.com",
        L"crazyegg.com", L"luckyorange.com", L"inspectlet.com", L"heapanalytics.com",
        L"kissmetrics.com", L"imrworldwide.com", L"browser-intake-datadoghq.com", L"addthis.com",
        // Russia
        L"mc.yandex.ru", L"an.yandex.ru", L"yandexadexchange.net", L"adfox.ru",
        L"top-fwz1.mail.ru", L"counter.yadro.ru",
        // Pop-under and aggressive networks
        L"popads.net", L"propellerads.com", L"propellerclick.com", L"exoclick.com", L"juicyads.com",
        L"trafficjunky.net", L"adcash.com", L"hilltopads.net", L"popcash.net", L"adsterra.com",
        L"a-ads.com", L"clickadu.com", L"onclickads.net", L"zeropark.com"
    };

    // Ad/tracker paths on any host (narrow patterns only).
    m_keywords = {
        L"/pagead/js/", L"/pagead/show_ads.js", L"/advert.js", L"/ads.js",
        L"/google-analytics.com/analytics.js", L"/gtag/js?id=", L"/gtm.js?id=", L"/hm.js?",
        L"/beacon.js", L"/pixel.gif", L"/pixel.png"
    };

    // Ad/tracker endpoints on hosts that otherwise serve real content.
    m_hostPaths = {
        {L"facebook.com", L"/tr"},
        {L"google.com", L"/pagead/"},
        {L"youtube.com", L"/pagead/"},
        {L"youtube.com", L"/api/stats/ads"},
        {L"youtube.com", L"/ptracking"},
        {L"t.co", L"/i/adsct"},
        {L"twitter.com", L"/i/adsct"},
        {L"x.com", L"/i/adsct"},
    };
}

const std::wstring* Rules::MatchHost(const std::wstring& host) const {
    if (host.empty()) return nullptr;
    auto it = m_domains.find(host);
    if (it != m_domains.end()) return &*it;
    for (size_t dot = host.find(L'.'); dot != std::wstring::npos; dot = host.find(L'.', dot + 1)) {
        it = m_domains.find(host.substr(dot + 1));
        if (it != m_domains.end()) return &*it;
    }
    return nullptr;
}

bool Rules::MatchPathKeyword(const std::wstring& lowerPath) const {
    for (const auto& kw : m_keywords) {
        if (lowerPath.find(kw) != std::wstring::npos) return true;
    }
    return false;
}

bool Rules::MatchHostPath(const std::wstring& host, const std::wstring& lowerPath) const {
    for (const auto& rule : m_hostPaths) {
        if (!IsUnder(host, rule.host) || !StartsWith(lowerPath, rule.prefix)) continue;
        // "/tr" matches "/tr", "/tr/…" and "/tr?…" but not "/translate".
        if (rule.prefix.back() == L'/' || lowerPath.size() == rule.prefix.size()) return true;
        const wchar_t next = lowerPath[rule.prefix.size()];
        if (next == L'/' || next == L'?') return true;
    }
    return false;
}

Decision Rules::Evaluate(const std::wstring& uri, const std::wstring& topHost, Resource kind) const {
    if (uri.empty() || !IsWebScheme(uri)) return {};
    const std::wstring host = ExtractHost(uri);
    if (host.empty()) return {};

    // First-party frames and images are never blocked (site logos, own embeds).
    if ((kind == Resource::Image || kind == Resource::Document) && !topHost.empty() && !IsThirdParty(host, topHost)) {
        return {};
    }

    const std::wstring path = LowerPath(uri);
    bool block = false;
    if (const std::wstring* entry = MatchHost(host)) {
        // Visiting a listed site itself (an analytics vendor's dashboard, say)
        // must keep working; its trackers on other sites stay blocked.
        block = topHost.empty() || !IsUnder(topHost, *entry);
    }
    if (!block) block = MatchHostPath(host, path) || MatchPathKeyword(path);
    if (!block) return {};

    Decision d;
    d.block = true;
    if (kind == Resource::Script) d.surrogate = SurrogateFor(host, path);
    return d;
}

std::vector<std::wstring> Rules::RequestFilterPatterns() const {
    std::vector<std::wstring> patterns;
    // "*://*example.com/*" covers example.com and every subdomain with one filter
    // (it also over-matches "notexample.com", which Evaluate then lets through).
    // An entry whose parent is listed too is already covered by the parent.
    for (const auto& domain : m_domains) {
        bool covered = false;
        for (size_t dot = domain.find(L'.'); dot != std::wstring::npos && !covered; dot = domain.find(L'.', dot + 1)) {
            const std::wstring parent = domain.substr(dot + 1);
            covered = parent.find(L'.') != std::wstring::npos && m_domains.count(parent) != 0;
        }
        if (!covered) patterns.push_back(L"*://*" + domain + L"/*");
    }
    for (const auto& rule : m_hostPaths) patterns.push_back(L"*://*" + rule.host + rule.prefix + L"*");
    for (const auto& kw : m_keywords) patterns.push_back(L"*" + kw + L"*");
    std::sort(patterns.begin(), patterns.end());
    return patterns;
}

std::wstring CosmeticScript(bool enabled, const std::vector<std::wstring>& disabledHosts) {
    std::wstring config = L"{\"enabled\":";
    config += enabled ? L"true" : L"false";
    config += L",\"off\":[";
    for (size_t i = 0; i < disabledHosts.size(); ++i) {
        if (i) config += L",";
        config += JsonString(Lower(disabledHosts[i]));
    }
    config += L"]}";

    std::wstring script = Widen(kCosmeticScript);
    const std::wstring marker = L"__ULB_CONFIG__";
    const auto pos = script.find(marker);
    if (pos != std::wstring::npos) script.replace(pos, marker.size(), config);
    return script;
}

} // namespace UltraLight::AdBlock
