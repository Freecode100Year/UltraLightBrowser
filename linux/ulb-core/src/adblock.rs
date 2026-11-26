use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceKind {
    Document = 0,
    Image = 1,
    Script = 2,
    Other = 3,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Decision {
    pub block: bool,
    pub surrogate: Option<&'static str>,
}

const SURROGATE_ANALYTICS: &str = r#"(function(){'use strict';
var noop=function(){};
function Tracker(){}
Tracker.prototype.get=function(){};Tracker.prototype.set=noop;Tracker.prototype.send=function(){fire(arguments);};
function fire(args){for(var i=0;i<args.length;i++){var o=args[i];if(o&&typeof o==='object'&&typeof o.hitCallback==='function'){try{o.hitCallback();}catch(e){}}}}
var ga=function(){var a=arguments;if(!a.length)return;if(typeof a[0]==='function'){try{a[0](new Tracker());}catch(e){}return;}fire(a);};
ga.create=function(){return new Tracker();};ga.getByName=function(){return new Tracker();};
ga.getAll=function(){return [];};ga.remove=noop;ga.loaded=true;ga.l=Date.now();
var name=window.GoogleAnalyticsObject||'ga';var old=window[name];window[name]=ga;
var q=old&&old.q;if(q&&q.length){for(var i=0;i<q.length;i++){try{ga.apply(null,q[i]);}catch(e){}}}
})();"#;

const SURROGATE_GTAG: &str = r#"(function(){'use strict';
var dl=window.dataLayer=window.dataLayer||[];
function later(f){if(typeof f==='function'){setTimeout(function(){try{f();}catch(e){}},1);}}
function run(o){if(!o||typeof o!=='object')return;later(o.eventCallback);
if(o.length>=3&&o[0]==='event'&&o[2]&&typeof o[2]==='object')later(o[2].event_callback);}
for(var i=0;i<dl.length;i++)run(dl[i]);
var push=dl.push;dl.push=function(){for(var i=0;i<arguments.length;i++)run(arguments[i]);return push.apply(this,arguments);};
if(typeof window.gtag!=='function'){window.gtag=function(){dl.push(arguments);};}
window.google_tag_manager=window.google_tag_manager||{};
})();"#;

const SURROGATE_GPT: &str = r#"(function(){'use strict';
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
})();"#;

const SURROGATE_ADS_BY_GOOGLE: &str = r#"(function(){'use strict';
window.adsbygoogle={loaded:true,push:function(){}};
})();"#;

const COSMETIC_SCRIPT: &str = r#"(function(){'use strict';
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
  setInterval(function(){
    var p=document.querySelector('.html5-video-player.ad-showing');
    if(!p)return;
    var v=p.querySelector('video.html5-main-video')||p.querySelector('video');
    if(v&&isFinite(v.duration)&&v.duration>0&&v.currentTime<v.duration-0.1){try{v.currentTime=v.duration;}catch(e){}}
    var b=p.querySelector('.ytp-skip-ad-button,.ytp-ad-skip-button,.ytp-ad-skip-button-modern,.ytp-ad-skip-button-slot button');
    if(b)b.click();
  },300);
})();
})();"#;

pub use ulb_adblock::{
    base_domain, domains, extract_host, is_third_party, match_host, request_filter_patterns,
};

pub fn evaluate(uri: &str, top_host: &str, kind: ResourceKind) -> Decision {
    let code = ulb_adblock::evaluate(uri, top_host, kind as i32);
    let surrogate = match (code >> 8) & 0xFF {
        1 => Some(SURROGATE_ANALYTICS),
        2 => Some(SURROGATE_GTAG),
        3 => Some(SURROGATE_GPT),
        4 => Some(SURROGATE_ADS_BY_GOOGLE),
        _ => None,
    };
    Decision {
        block: (code & 1) != 0,
        surrogate,
    }
}

pub fn cosmetic_script(enabled: bool, disabled_hosts: &[String]) -> String {
    let off: Vec<String> = disabled_hosts
        .iter()
        .map(|h| h.to_ascii_lowercase())
        .collect();
    let cfg = serde_json::json!({
        "enabled": enabled,
        "off": off,
    });
    let mut s = serde_json::to_string(&cfg).unwrap_or_else(|_| "{\"enabled\":false,\"off\":[]}".into());
    // Escape `<` and `>` as \u003c / \u003e for safe embedding inside scripts/HTML.
    if s.contains('<') || s.contains('>') {
        s = s.replace('<', "\\u003c").replace('>', "\\u003e");
    }
    let _ = Value::Null;
    COSMETIC_SCRIPT.replace("__ULB_CONFIG__", &s)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn glob_match(pattern: &str, text: &str) -> bool {
        let p = pattern.as_bytes();
        let t = text.as_bytes();
        let (mut pi, mut ti, mut star, mut mark) = (0, 0, None, 0);
        while ti < t.len() {
            if pi < p.len() && p[pi] == t[ti] {
                pi += 1;
                ti += 1;
            } else if pi < p.len() && p[pi] == b'*' {
                star = Some(pi);
                pi += 1;
                mark = ti;
            } else if let Some(st) = star {
                pi = st + 1;
                mark += 1;
                ti = mark;
            } else {
                return false;
            }
        }
        while pi < p.len() && p[pi] == b'*' {
            pi += 1;
        }
        pi == p.len()
    }

    #[test]
    fn adblock_rules_surrogates_and_cosmetic_config() {
        assert_eq!(
            extract_host("https://User:Pw@WWW.DoubleClick.NET.:443/x?y#z"),
            "www.doubleclick.net"
        );
        assert_eq!(base_domain("news.bbc.co.uk"), "bbc.co.uk");
        assert!(evaluate("https://hm.baidu.com/hm.gif?x", "www.example.cn", ResourceKind::Image).block);
        assert!(
            !evaluate(
                "https://www.example.com/img/pixel.gif",
                "www.example.com",
                ResourceKind::Image
            )
            .block
        );
        assert!(
            evaluate(
                "https://www.google-analytics.com/analytics.js",
                "a.com",
                ResourceKind::Script
            )
            .surrogate
            .is_some()
        );
        assert!(
            evaluate(
                "https://www.google-analytics.com/analytics.js",
                "a.com",
                ResourceKind::Other
            )
            .surrogate
            .is_none()
        );
        let patterns = request_filter_patterns();
        let sample = "https://www.youtube.com/api/stats/ads?x";
        assert!(evaluate(sample, "www.youtube.com", ResourceKind::Other).block);
        assert!(patterns.iter().any(|p| glob_match(p, sample)));
        assert!(!patterns
            .iter()
            .any(|p| glob_match(p, "https://www.example.com/index.html")));

        let off = cosmetic_script(false, &[]);
        assert!(off.contains("var CFG={\"enabled\":false,\"off\":[]};"));
        let on = cosmetic_script(true, &["News.Example.com".into(), "a\"b<c".into()]);
        assert!(on.contains("\"off\":[\"news.example.com\",\"a\\\"b\\u003cc\"]"));
        assert!(!on.contains("__ULB_CONFIG__"));
    }
}
