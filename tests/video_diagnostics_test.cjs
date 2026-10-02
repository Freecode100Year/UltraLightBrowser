const fs=require('fs'),vm=require('vm'),assert=require('assert');
const path=require('path');
const source=fs.readFileSync(path.join(__dirname,'../src/VideoDiagnostics.hpp'),'utf8').match(/LR"video\(([\s\S]*?)\)video"/)[1];
let now=0,total=100,dropped=2;
const video={videoWidth:3840,videoHeight:2160,paused:false,seeking:false,currentTime:5,currentSrc:'movie',playbackRate:1,readyState:4,buffered:{length:1,start:()=>0,end:()=>30},getVideoPlaybackQuality:()=>({totalVideoFrames:total,droppedVideoFrames:dropped})};
const ctx={window:{},document:{querySelectorAll:()=>[video]},performance:{now:()=>now},WeakMap};
function run(){return vm.runInNewContext(source,ctx);}
assert(run().includes('再次打开'));
now=10000;total=700;dropped=8;video.currentTime=15;
let report=run();assert(report.includes('1.00%'));assert(report.includes('59.40'));assert(report.includes('15.00'));assert(report.includes('3840 × 2160'));
now=20000;total=5;dropped=0;assert(run().includes('重新建立'));
video.getVideoPlaybackQuality=undefined;assert(run().includes('不支持帧统计'));
ctx.document.querySelectorAll=()=>[];assert(run().includes('未发现视频'));
console.log('Video diagnostic regression tests passed');
