#pragma once
namespace UltraLight {
// On-demand snapshots only: no persistent timer or per-frame JS work.
inline constexpr const wchar_t* kVideoDiagnosticsScript = LR"video((() => {
    const api = window.__UltraLightSurround;
    const lines = [];
    if (api && api.getStatus) {
        const status = api.getStatus();
        lines.push('音频输出：' + (status.output === 'native' ? '原声 / 系统处理' : '浏览器增强') +
            '，处理上下文：' + (status.audioContext === 'not-created' ? '未创建' : status.audioContext));
    }
    // Non-enumerable so the sample store does not show up in page global scans.
    if (!window.__UltraLightVideoSamples) {
        Object.defineProperty(window, '__UltraLightVideoSamples', {value: new WeakMap()});
    }
    const samples = window.__UltraLightVideoSamples;
    const now = performance.now();
    const videos = Array.from(document.querySelectorAll('video'));
    if (!videos.length) lines.push('当前主文档未发现视频。');
    videos.forEach((v, i) => {
        lines.push('视频 ' + (i + 1) + '：' + v.videoWidth + ' × ' + v.videoHeight +
            '，' + (v.paused ? '暂停' : '播放') + '，readyState=' + v.readyState);
        let ahead = 0;
        for (let n = 0; n < v.buffered.length; ++n) {
            if (v.buffered.start(n) <= v.currentTime && v.buffered.end(n) >= v.currentTime) {
                ahead = v.buffered.end(n) - v.currentTime;
                break;
            }
        }
        lines.push('当前位置缓冲余量：' + ahead.toFixed(2) + ' 秒');
        if (!v.getVideoPlaybackQuality) { lines.push('不支持帧统计'); return; }
        const q = v.getVideoPlaybackQuality();
        const current = {time:now, total:q.totalVideoFrames, dropped:q.droppedVideoFrames,
            source:v.currentSrc, width:v.videoWidth, height:v.videoHeight, mediaTime:v.currentTime,
            paused:v.paused, rate:v.playbackRate};
        const old = samples.get(v);
        samples.set(v, current);
        lines.push('累计帧：' + current.total + '，累计丢帧：' + current.dropped);
        const seconds = old ? (now - old.time) / 1000 : 0;
        if (!old) {
            lines.push('保持播放，约 30 秒后再次打开诊断以查看区间数据。');
        } else if (seconds < 1 || old.source !== current.source || old.width !== current.width ||
            old.height !== current.height || current.total < old.total || current.dropped < old.dropped ||
            current.mediaTime < old.mediaTime || v.seeking || old.paused || v.paused || old.rate !== current.rate) {
            lines.push('状态或计数变化，已重新建立采样基线。');
        } else {
            const total = current.total - old.total;
            const dropped = current.dropped - old.dropped;
            if (total <= 0 || dropped > total) lines.push('区间帧数据不足，不能判断是否流畅。');
            else lines.push('区间 ' + seconds.toFixed(1) + ' 秒：总帧 ' + total + '，丢帧 ' + dropped +
                '，丢帧率 ' + (100 * dropped / total).toFixed(2) + '%，显示帧/秒约 ' + ((total - dropped) / seconds).toFixed(2));
        }
    });
    lines.push('只统计主文档视频；跨域 iframe、硬解状态、编码格式和网络停顿原因需用站点统计或 DevTools 核实。');
    lines.push('区间含暂停、跳转或缓冲等待时，帧/秒不能当作源视频帧率。丢帧率不能单独证明无卡顿。');
    lines.push('此诊断不能确认 Dolby Atmos 已启用。');
    return lines.join('\n');
})())video";
}
