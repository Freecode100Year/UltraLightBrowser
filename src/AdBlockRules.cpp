#include "AdBlockRules.hpp"
#include <cstdint>
#include <cwctype>
#include <string_view>
extern "C" {
size_t ulb_domain_count(); size_t ulb_domain_at(size_t,char*,size_t);
size_t ulb_pattern_count(); size_t ulb_pattern_at(size_t,char*,size_t);
size_t ulb_extract_host(const char*,size_t,char*,size_t);
size_t ulb_base_domain(const char*,size_t,char*,size_t);
size_t ulb_match_host(const char*,size_t,char*,size_t);
int32_t ulb_evaluate(const char*,size_t,const char*,size_t,int32_t);
}
namespace UltraLight::AdBlock { namespace {
std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

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
})();)ULBJS";std::wstring Widen(const char* s) {
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

std::string WideToUtf8(const std::wstring& input) {
    std::string out;
    for(size_t i=0;i<input.size();++i){uint32_t cp=static_cast<uint32_t>(input[i]);
        if constexpr(sizeof(wchar_t)==2){if(cp>=0xd800&&cp<=0xdbff&&i+1<input.size()){uint32_t lo=static_cast<uint32_t>(input[++i]);cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);}}
        if(cp<=0x7f)out.push_back(static_cast<char>(cp));else if(cp<=0x7ff){out.push_back(static_cast<char>(0xc0|(cp>>6)));out.push_back(static_cast<char>(0x80|(cp&63)));}else if(cp<=0xffff){out.push_back(static_cast<char>(0xe0|(cp>>12)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}else{out.push_back(static_cast<char>(0xf0|(cp>>18)));out.push_back(static_cast<char>(0x80|((cp>>12)&63)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}}
    return out;
}
std::wstring Utf8ToWide(std::string_view s){std::wstring out;for(size_t i=0;i<s.size();){uint32_t cp=static_cast<unsigned char>(s[i++]);unsigned n=0;if(cp<0x80){}else if((cp&0xe0)==0xc0){cp&=31;n=1;}else if((cp&0xf0)==0xe0){cp&=15;n=2;}else if((cp&0xf8)==0xf0){cp&=7;n=3;}else cp=0xfffd;while(n--&&i<s.size())cp=(cp<<6)|(static_cast<unsigned char>(s[i++])&63);if constexpr(sizeof(wchar_t)==2){if(cp>0xffff){cp-=0x10000;out.push_back(static_cast<wchar_t>(0xd800+(cp>>10)));cp=0xdc00+(cp&1023);}}out.push_back(static_cast<wchar_t>(cp));}return out;}
template<typename Fn> std::wstring ReadRustString(Fn fn){size_t n=fn(nullptr,0);std::string b(n,'\0');if(n)fn(b.data(),b.size());return Utf8ToWide(b);}
}
std::wstring ExtractHost(const std::wstring& uri){auto s=WideToUtf8(uri);return ReadRustString([&](char* o,size_t n){return ulb_extract_host(s.data(),s.size(),o,n);});}
std::wstring BaseDomain(const std::wstring& host){auto s=WideToUtf8(host);return ReadRustString([&](char* o,size_t n){return ulb_base_domain(s.data(),s.size(),o,n);});}
bool IsThirdParty(const std::wstring& req,const std::wstring& top){return req.empty()||top.empty()||(req!=top&&BaseDomain(req)!=BaseDomain(top));}
const Rules& Rules::Default(){static const Rules r;return r;}
Rules::Rules(){for(size_t i=0;i<ulb_domain_count();++i){std::string b(256,'\0');auto n=ulb_domain_at(i,b.data(),b.size());b.resize(n);m_domains.insert(Utf8ToWide(b));}}
const std::wstring* Rules::MatchHost(const std::wstring& host)const{auto s=WideToUtf8(host);std::string b(256,'\0');auto n=ulb_match_host(s.data(),s.size(),b.data(),b.size());if(!n)return nullptr;b.resize(n);auto i=m_domains.find(Utf8ToWide(b));return i==m_domains.end()?nullptr:&*i;}
Decision Rules::Evaluate(const std::wstring& uri,const std::wstring& top,Resource kind)const{auto u=WideToUtf8(uri),t=WideToUtf8(top);int c=ulb_evaluate(u.data(),u.size(),t.data(),t.size(),static_cast<int>(kind));Decision d;d.block=(c&1)!=0;switch((c>>8)&255){case 1:d.surrogate=kSurrogateAnalytics;break;case 2:d.surrogate=kSurrogateGtag;break;case 3:d.surrogate=kSurrogateGpt;break;case 4:d.surrogate=kSurrogateAdsByGoogle;break;default:break;}return d;}
std::vector<std::wstring> Rules::RequestFilterPatterns()const{std::vector<std::wstring> v;for(size_t i=0;i<ulb_pattern_count();++i){std::string b(512,'\0');auto n=ulb_pattern_at(i,b.data(),b.size());b.resize(n);v.push_back(Utf8ToWide(b));}return v;}
std::wstring CosmeticScript(bool enabled,const std::vector<std::wstring>& disabledHosts){std::wstring c=L"{\"enabled\":";c+=enabled?L"true":L"false";c+=L",\"off\":[";for(size_t i=0;i<disabledHosts.size();++i){if(i)c+=L",";c+=JsonString(Lower(disabledHosts[i]));}c+=L"]}";std::wstring s=Widen(kCosmeticScript);const std::wstring m=L"__ULB_CONFIG__";auto p=s.find(m);if(p!=s.npos)s.replace(p,m.size(),c);return s;}
} // namespace UltraLight::AdBlock
