use crate::audio_endpoint::is_headphone_endpoint;
use crate::config::AppSettings;
use serde_json::json;

/// Returns the Web Audio DSP + True-Peak Limiter injection script matching `WebViewManager.cpp`.
pub fn build_audio_dsp_script(settings: &AppSettings, pipewire_headphones: bool) -> String {
    let effective_device = match settings.audio_device_mode.as_str() {
        "headphones" => "headphones",
        "speakers" => "speakers",
        _ => {
            if pipewire_headphones || is_headphone_endpoint(false, "", "") {
                "headphones"
            } else {
                "speakers"
            }
        }
    };

    let init_cfg = json!({
        "nativeOutput": settings.system_audio_passthrough,
        "enabled": settings.enable_surround_sound,
        "mode": settings.surround_sound_mode,
        "device": effective_device,
        "deEsser": settings.enable_de_esser,
        "nightMode": settings.enable_night_mode,
        "vocalBoost": settings.enable_vocal_boost,
        "volumeBoost": settings.audio_volume_boost,
        "monoDownmix": settings.enable_mono_downmix,
    });

    format!(
        r#"(function() {{
    if (window.__UltraLightSurroundInstalled) return;
    window.__UltraLightSurroundInstalled = true;
    let cfg = {init_cfg};
{body}
}})();"#,
        init_cfg = init_cfg,
        body = DSP_CORE_BODY
    )
}

/// Builds a live runtime config update snippet when settings or PipeWire endpoints change.
pub fn build_audio_update_script(settings: &AppSettings, pipewire_headphones: bool) -> String {
    let effective_device = match settings.audio_device_mode.as_str() {
        "headphones" => "headphones",
        "speakers" => "speakers",
        _ => {
            if pipewire_headphones {
                "headphones"
            } else {
                "speakers"
            }
        }
    };
    let payload = json!({
        "nativeOutput": settings.system_audio_passthrough,
        "enabled": settings.enable_surround_sound,
        "mode": settings.surround_sound_mode,
        "device": effective_device,
        "deEsser": settings.enable_de_esser,
        "nightMode": settings.enable_night_mode,
        "vocalBoost": settings.enable_vocal_boost,
        "volumeBoost": settings.audio_volume_boost,
        "monoDownmix": settings.enable_mono_downmix,
    });
    format!(
        "try {{ window.__UltraLightSurround && window.__UltraLightSurround.updateConfig({payload}); }} catch (e) {{}}"
    )
}

const DSP_CORE_BODY: &str = r#"
    const PRESETS = {
        dialogue: { width: 1.0, crossfeed: 0.0, reverbAmount: 0.0 },
        off: { width: 1.0, crossfeed: 0.0, reverbAmount: 0.0 },
        light: { width: 1.15, crossfeed: 0.12, reverbAmount: 0.06 },
        standard: { width: 1.35, crossfeed: 0.20, reverbAmount: 0.10 },
        cinema: { width: 1.55, crossfeed: 0.25, reverbAmount: 0.16 }
    };
    const CROSSFEED_PARAMS = {
        light: { cutoff: 800, level: 0.25, compFreq: 520, compDb: -2.35 },
        standard: { cutoff: 700, level: 0.40, compFreq: 470, compDb: -3.65 },
        cinema: { cutoff: 620, level: 0.55, compFreq: 430, compDb: -4.80 }
    };
    const DEESS_CROSSOVER_HZ = 5500;
    const BUTTERWORTH_Q_DB = -3.01029995664;
    const EQ_HEADROOM_DB = 0.5;
    const attachedElements = new WeakMap();
    const pageClaimedElements = new WeakSet();
    const pendingPlayElements = new WeakSet();
    let audioCtx = null;
    let cachedReverbBuffer = null;
    let idleTimer = null;

    try {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (!cfg.nativeOutput && AC && AC.prototype && AC.prototype.createMediaElementSource) {
            const origCreate = AC.prototype.createMediaElementSource;
            AC.prototype.createMediaElementSource = function(mediaEl) {
                if (mediaEl) {
                    pageClaimedElements.add(mediaEl);
                    const existing = attachedElements.get(mediaEl);
                    if (existing && existing.release) {
                        try { existing.release(); } catch (e) {}
                        attachedElements.delete(mediaEl);
                    }
                }
                return origCreate.apply(this, arguments);
            };
        }
    } catch (e) {}

    function getAudioContext() {
        if (cfg.nativeOutput) return null;
        if (!audioCtx) {
            const AudioContextClass = window.AudioContext || window.webkitAudioContext;
            if (!AudioContextClass) return null;
            try { audioCtx = new AudioContextClass({ latencyHint: "interactive" }); }
            catch (e) { try { audioCtx = new AudioContextClass(); } catch (e2) { return null; } }
            loadLimiter(audioCtx);
        }
        return audioCtx;
    }

    function ultraLightTruePeakLimiter() {
        const PHASES = 4, TAPS = 64, HALF = TAPS / 2;
        function besselI0(x) {
            let sum = 1, term = 1;
            for (let k = 1; k < 32; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
            return sum;
        }
        const COEF = [];
        let coefAbs = 1;
        for (let p = 1; p < PHASES; ++p) {
            const row = new Float64Array(TAPS);
            let sum = 0, abs = 0;
            for (let j = 0; j < TAPS; ++j) {
                const t = (j - (HALF - 1)) - p / PHASES;
                const w = t / HALF;
                const win = Math.abs(w) >= 1 ? 0 : besselI0(9 * Math.sqrt(1 - w * w)) / besselI0(9);
                row[j] = Math.sin(Math.PI * t) / (Math.PI * t) * win;
                sum += row[j];
            }
            for (let j = 0; j < TAPS; ++j) { row[j] /= sum; abs += Math.abs(row[j]); }
            if (abs > coefAbs) coefAbs = abs;
            COEF.push(row);
        }
        class TruePeakLimiter extends AudioWorkletProcessor {
            constructor(options) {
                super();
                const o = (options && options.processorOptions) || {};
                this.ceiling = Math.pow(10, (typeof o.ceilingDb === "number" ? o.ceilingDb : -1) / 20);
                this.quiet = this.ceiling / coefAbs;
                const L = Math.max(2, Math.round(sampleRate * (o.lookaheadMs || 3) / 1000));
                this.L = L;
                this.A = Math.ceil((L + 1) / 2);
                this.B = L + 1 - this.A;
                this.releaseCoef = Math.exp(-1 / (sampleRate * (o.releaseMs || 80) / 1000));
                this.latency = HALF + L - 1;
                let size = 1;
                while (size < this.latency + TAPS + 4) size <<= 1;
                this.size = size; this.mask = size - 1;
                this.hist = [];
                this.seg = new Float64Array(size);
                this.needs = new Float64Array(size).fill(1);
                this.dq = new Float64Array(size);
                this.dqHead = 0; this.dqTail = 0;
                this.loudAt = -1e9;
                this.boxA = new Float64Array(size).fill(1); this.sumA = this.A;
                this.boxB = new Float64Array(size).fill(1); this.sumB = this.B;
                this.count = 0; this.rel = 1; this.pos = 0;
                this.minGain = 1; this.clipped = 0;
            }
            process(inputs, outputs) {
                const input = inputs[0] || [], output = outputs[0];
                const channels = output.length, frames = output[0].length;
                while (this.hist.length < channels) this.hist.push(new Float32Array(this.size));
                const m = this.mask, L = this.L, A = this.A, B = this.B, hist = this.hist;
                for (let i = 0; i < frames; ++i) {
                    const n = this.pos;
                    let loud = false;
                    for (let c = 0; c < channels; ++c) {
                        const src = input[c] || input[0];
                        let v = src ? src[i] : 0;
                        if (v !== v || v === Infinity || v === -Infinity) v = 0;
                        hist[c][n & m] = v;
                        if (v > this.quiet || v < -this.quiet) loud = true;
                    }
                    if (loud) this.loudAt = n;
                    const q = n - HALF;
                    let peak = 0, seg = 0;
                    for (let c = 0; c < channels; ++c) {
                        const a = Math.abs(hist[c][q & m]);
                        if (a > peak) peak = a;
                    }
                    if (n - this.loudAt < TAPS) {
                        const base = q - (HALF - 1);
                        for (let c = 0; c < channels; ++c) {
                            const h = hist[c];
                            for (let p = 0; p < COEF.length; ++p) {
                                const row = COEF[p];
                                let s = 0;
                                for (let j = 0; j < TAPS; ++j) s += row[j] * h[(base + j) & m];
                                if (s < 0) s = -s;
                                if (s > seg) seg = s;
                            }
                        }
                    }
                    this.seg[q & m] = seg;
                    if (seg > peak) peak = seg;
                    const before = this.seg[(q - 1) & m];
                    if (before > peak) peak = before;
                    const need = peak > this.ceiling ? this.ceiling / peak : 1;
                    this.needs[q & m] = need;
                    if (need < this.minGain) this.minGain = need;
                    while (this.dqTail > this.dqHead && this.needs[this.dq[(this.dqTail - 1) & m] & m] >= need) --this.dqTail;
                    this.dq[this.dqTail++ & m] = q;
                    while (this.dq[this.dqHead & m] <= q - L) ++this.dqHead;
                    const held = this.needs[this.dq[this.dqHead & m] & m];
                    this.rel = held <= this.rel ? held : held + (this.rel - held) * this.releaseCoef;
                    this.sumA += this.rel - this.boxA[(q - A) & m];
                    this.boxA[q & m] = this.rel;
                    const ga = this.sumA / A;
                    this.sumB += ga - this.boxB[(q - B) & m];
                    this.boxB[q & m] = ga;
                    if (++this.count >= 8192) {
                        this.count = 0;
                        let s = 0; for (let k = 0; k < A; ++k) s += this.boxA[(q - k) & m]; this.sumA = s;
                        s = 0; for (let k = 0; k < B; ++k) s += this.boxB[(q - k) & m]; this.sumB = s;
                    }
                    const g = Math.min(1, this.sumB / B);
                    const out = (q - L + 1) & m;
                    for (let c = 0; c < channels; ++c) {
                        let y = hist[c][out] * g;
                        if (y > this.ceiling) { y = this.ceiling; ++this.clipped; }
                        else if (y < -this.ceiling) { y = -this.ceiling; ++this.clipped; }
                        output[c][i] = y;
                    }
                    this.pos = n + 1;
                }
                return true;
            }
        }
        registerProcessor("ultralight-true-peak-limiter", TruePeakLimiter);
    }

    const LIMITER_SOURCE = "(" + ultraLightTruePeakLimiter.toString() + ")();";
    const LIMITER_NAME = "ultralight-true-peak-limiter";
    const LIMITER_OPTIONS = { ceilingDb: -1, lookaheadMs: 3, releaseMs: 80 };
    const BlobClass = window.Blob;
    const createObjectURL = URL.createObjectURL.bind(URL);
    const revokeObjectURL = URL.revokeObjectURL.bind(URL);
    let limiterState = "not-loaded";

    function loadLimiter(ctx) {
        if (limiterState !== "not-loaded") return;
        if (!ctx.audioWorklet || typeof window.AudioWorkletNode !== "function" || !BlobClass) {
            limiterState = "unavailable"; return;
        }
        let url;
        try { url = createObjectURL(new BlobClass([LIMITER_SOURCE], { type: "text/javascript" })); }
        catch (e) { limiterState = "unavailable"; return; }
        limiterState = "loading";
        ctx.audioWorklet.addModule(url).then(() => {
            limiterState = "ready";
            refreshControllers();
        }, () => { limiterState = "unavailable"; }).then(() => revokeObjectURL(url));
    }

    function refreshControllers() {
        try {
            document.querySelectorAll("video, audio").forEach(el => {
                const c = attachedElements.get(el);
                if (c) c.update(cfg);
            });
        } catch (e) {}
    }

    function getSmallRoomBuffer(ctx) {
        if (cachedReverbBuffer && cachedReverbBuffer.sampleRate === ctx.sampleRate) return cachedReverbBuffer;
        const rate = ctx.sampleRate, duration = 0.035, numSamples = Math.floor(rate * duration);
        const buf = ctx.createBuffer(2, numSamples, rate);
        function noise(seed) {
            let a = seed >>> 0;
            return function() {
                a = (a + 0x6D2B79F5) >>> 0; let t = a;
                t = Math.imul(t ^ (t >>> 15), t | 1);
                t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
                return ((t ^ (t >>> 14)) >>> 0) / 4294967296 * 2 - 1;
            };
        }
        const predelay = Math.round(rate * 0.0015);
        function fill(data, seed) {
            const rnd = noise(seed); let lp = 0, lp2 = 0, energy = 0, target = 0;
            for (let i = 0; i < numSamples; ++i) {
                if (i < predelay) { data[i] = 0; continue; }
                const t = (i - predelay) / rate, env = Math.exp(-t / 0.009) * 0.5;
                const cutoff = 6000 * Math.pow(1500 / 6000, i / numSamples);
                const k = 1 - Math.exp(-2 * Math.PI * cutoff / rate);
                lp += k * (rnd() - lp); lp2 += k * (lp - lp2);
                data[i] = lp2 * env; energy += data[i] * data[i]; target += env * env / 3;
            }
            const scale = energy > 0 ? Math.sqrt(target / energy) : 0;
            for (let i = predelay; i < numSamples; ++i) data[i] *= scale;
        }
        fill(buf.getChannelData(0), 0x51A7E5);
        fill(buf.getChannelData(1), 0x2C0FFEE);
        cachedReverbBuffer = buf;
        return buf;
    }

    function isSafeUrl(urlStr, el) {
        if (!urlStr || typeof urlStr !== "string") return false;
        if (urlStr.startsWith("blob:") || urlStr.startsWith("data:")) return true;
        try { if (new URL(urlStr, window.location.href).origin === window.location.origin) return true; } catch (e) { return false; }
        return !!(el && el.crossOrigin && (el.crossOrigin === "anonymous" || el.crossOrigin === "use-credentials"));
    }

    function canProcessElement(el) {
        if (!el || attachedElements.has(el) || pageClaimedElements.has(el) || el.mediaKeys) return false;
        const src = el.currentSrc || el.src;
        if (src) return isSafeUrl(src, el);
        const sources = el.querySelectorAll ? el.querySelectorAll("source") : [];
        if (sources.length > 0) {
            let any = false;
            for (let i = 0; i < sources.length; ++i) {
                if (sources[i].src) { if (isSafeUrl(sources[i].src, el)) any = true; else return false; }
            }
            return any;
        }
        return false;
    }

    function wakeAudioContext() {
        if (idleTimer) { clearTimeout(idleTimer); idleTimer = null; }
        const ctx = getAudioContext();
        if (ctx && ctx.state === "suspended") ctx.resume().catch(() => {});
    }

    function scheduleIdleSuspend() {
        let anyPlaying = false;
        try { document.querySelectorAll("video, audio").forEach(m => { if (!m.paused && !m.ended && m.readyState > 2) anyPlaying = true; }); } catch (e) {}
        if (!anyPlaying && !idleTimer) {
            idleTimer = setTimeout(() => {
                idleTimer = null;
                const ctx = getAudioContext();
                if (ctx && ctx.state === "running") ctx.suspend().catch(() => {});
            }, 1000);
        }
    }

    function needsProcessing() {
        return !cfg.nativeOutput && (cfg.enabled || cfg.vocalBoost || cfg.monoDownmix || cfg.deEsser || cfg.nightMode || cfg.volumeBoost > 1.0);
    }

    function trySetupSurround(el) {
        if (!needsProcessing() || !canProcessElement(el)) return;
        const ctx = getAudioContext();
        if (!ctx) return;
        wakeAudioContext();
        if (ctx.state !== "running") {
            pendingPlayElements.add(el);
            ctx.resume().then(() => {
                if (ctx.state === "running" && pendingPlayElements.has(el) && !el.paused) {
                    pendingPlayElements.delete(el);
                    setupSurroundForElement(el);
                }
            }).catch(() => {});
            return;
        }
        setupSurroundForElement(el);
    }

    function setupSurroundForElement(el) {
        if (!needsProcessing() || !canProcessElement(el)) return;
        const ctx = getAudioContext();
        if (!ctx || ctx.state !== "running") return;
        let sourceNode;
        try { sourceNode = ctx.createMediaElementSource(el); } catch (e) { return; }
        let graphNodes = [], preamp = null, vocal = null, topology = "", rebuildTimer = null;
        const output = ctx.createGain();
        output.connect(ctx.destination);
        function smooth(param, value) { param.cancelScheduledValues(ctx.currentTime); param.setTargetAtTime(value, ctx.currentTime, 0.015); }
        function make(method, ...args) { const n = ctx[method](...args); graphNodes.push(n); return n; }
        function gain(value) { const n = make("createGain"); n.gain.value = value; return n; }
        function clearGraph() { sourceNode.disconnect(); for (const n of graphNodes) { try { n.disconnect(); } catch (e) {} } graphNodes = []; preamp = null; vocal = null; }
        function voiceDb() { return (cfg.vocalBoost || (cfg.enabled && cfg.mode === "dialogue")) ? 4.5 : 0; }
        function preampValue() {
            const req = Math.max(1, Math.min(3, Number(cfg.volumeBoost) || 1));
            const trim = voiceDb() > 0 ? voiceDb() + EQ_HEADROOM_DB : 0;
            return req * Math.pow(10, -trim / 20);
        }
        function build() {
            clearGraph();
            if (!needsProcessing()) { sourceNode.connect(output); return; }
            preamp = gain(preampValue());
            preamp.channelCount = 2; preamp.channelCountMode = "explicit"; preamp.channelInterpretation = "speakers";
            sourceNode.connect(preamp);
            vocal = make("createBiquadFilter"); vocal.type = "peaking"; vocal.frequency.value = 3000; vocal.Q.value = 1.2; vocal.gain.value = voiceDb();
            preamp.connect(vocal);
            let toneOutput = vocal;
            if (cfg.deEsser) {
                const merge = gain(1);
                function xover(t) {
                    const a = make("createBiquadFilter"), b = make("createBiquadFilter");
                    a.type = b.type = t; a.frequency.value = b.frequency.value = DEESS_CROSSOVER_HZ; a.Q.value = b.Q.value = BUTTERWORTH_Q_DB;
                    vocal.connect(a); a.connect(b); return b;
                }
                const lowDelay = make("createDelay", 0.02); lowDelay.delayTime.value = 0.006;
                xover("lowpass").connect(lowDelay); lowDelay.connect(merge);
                const deEsser = make("createDynamicsCompressor");
                deEsser.threshold.value = -32; deEsser.knee.value = 6; deEsser.ratio.value = 4; deEsser.attack.value = 0.002; deEsser.release.value = 0.06;
                xover("highpass").connect(deEsser); deEsser.connect(merge); toneOutput = merge;
            }
            const splitter = make("createChannelSplitter", 2); toneOutput.connect(splitter);
            const mid = gain(1), side = gain(1);
            const midL = gain(0.5), midR = gain(0.5), sideL = gain(0.5), sideR = gain(-0.5);
            splitter.connect(midL, 0); splitter.connect(midR, 1); midL.connect(mid); midR.connect(mid);
            splitter.connect(sideL, 0); splitter.connect(sideR, 1); sideL.connect(side); sideR.connect(side);
            const preset = cfg.enabled ? (PRESETS[cfg.mode] || PRESETS.standard) : PRESETS.off;
            const left = gain(1), right = gain(1); mid.connect(left); mid.connect(right);
            if (!cfg.monoDownmix) {
                let sideInput = side;
                if (cfg.enabled && cfg.mode !== "dialogue") {
                    const hp = make("createBiquadFilter"); hp.type = "highpass"; hp.frequency.value = 150; hp.Q.value = BUTTERWORTH_Q_DB;
                    side.connect(hp); sideInput = hp;
                }
                const wl = gain(preset.width), wr = gain(-preset.width);
                sideInput.connect(wl); sideInput.connect(wr); wl.connect(left); wr.connect(right);
            }
            let program;
            if (cfg.enabled && cfg.device !== "speakers" && cfg.mode !== "dialogue") {
                const cL = gain(1), cR = gain(1); left.connect(cL); right.connect(cR);
                const cf = CROSSFEED_PARAMS[cfg.mode] || CROSSFEED_PARAMS.standard;
                function crossfeed(f, t) {
                    const fl = make("createBiquadFilter"); fl.type = "lowpass"; fl.frequency.value = cf.cutoff; fl.Q.value = BUTTERWORTH_Q_DB;
                    const mx = gain(cf.level); f.connect(fl); fl.connect(mx); mx.connect(t);
                }
                crossfeed(left, cR); crossfeed(right, cL);
                function comp(f) { const sh = make("createBiquadFilter"); sh.type = "lowshelf"; sh.frequency.value = cf.compFreq; sh.gain.value = cf.compDb; f.connect(sh); return sh; }
                function pan(f, x, z) { const p = make("createPanner"); p.panningModel = "HRTF"; p.positionX.value = x; p.positionY.value = 0; p.positionZ.value = z; f.connect(p); return p; }
                program = gain(1);
                pan(comp(cL), -0.5, -0.866).connect(program);
                pan(comp(cR), 0.5, -0.866).connect(program);
                if (cfg.mode === "cinema" && !cfg.monoDownmix) {
                    const d = make("createDelay", 0.05); d.delayTime.value = 0.015; const rear = gain(0.20);
                    side.connect(d); d.connect(rear);
                    pan(rear, -0.94, 0.34).connect(program); pan(rear, 0.94, 0.34).connect(program);
                }
            } else {
                program = make("createChannelMerger", 2); left.connect(program, 0, 0); right.connect(program, 0, 1);
            }
            let finalMix = program;
            if (cfg.enabled && cfg.mode === "cinema") {
                const mix = gain(1), conv = make("createConvolver"); conv.buffer = getSmallRoomBuffer(ctx);
                const wet = gain(preset.reverbAmount); program.connect(mix); program.connect(conv); conv.connect(wet); wet.connect(mix); finalMix = mix;
            }
            if (cfg.nightMode) {
                const night = make("createDynamicsCompressor");
                night.threshold.value = -24; night.knee.value = 18; night.ratio.value = 3; night.attack.value = 0.015; night.release.value = 0.24;
                finalMix.connect(night); finalMix = night;
            }
            const dialogue = cfg.enabled && cfg.mode === "dialogue";
            if (limiterState === "ready") {
                let lim = null;
                try { lim = new window.AudioWorkletNode(ctx, LIMITER_NAME, { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: "explicit", channelInterpretation: "speakers", processorOptions: LIMITER_OPTIONS }); } catch (e) {}
                if (lim) {
                    graphNodes.push(lim);
                    let feed = finalMix;
                    if (dialogue) {
                        const lev = make("createDynamicsCompressor"); lev.threshold.value = -18; lev.knee.value = 6; lev.ratio.value = 3; lev.attack.value = 0.003; lev.release.value = 0.10;
                        feed.connect(lev); feed = lev;
                    }
                    feed.connect(lim); lim.connect(output); return;
                }
            }
            const cmp = make("createDynamicsCompressor");
            cmp.threshold.value = dialogue ? -18 : -3; cmp.knee.value = dialogue ? 6 : 3; cmp.ratio.value = dialogue ? 3 : 12; cmp.attack.value = 0.003; cmp.release.value = 0.10;
            finalMix.connect(cmp); cmp.connect(output);
        }
        function key() { return JSON.stringify([needsProcessing(), cfg.enabled, cfg.mode, cfg.device, cfg.monoDownmix, cfg.deEsser, cfg.nightMode, limiterState === "ready"]); }
        const controller = {
            update() {
                const next = key();
                if (next !== topology) {
                    topology = next;
                    if (rebuildTimer) clearTimeout(rebuildTimer);
                    smooth(output.gain, 0);
                    rebuildTimer = setTimeout(() => { rebuildTimer = null; try { build(); } catch (e) { clearGraph(); sourceNode.connect(output); } smooth(output.gain, 1); }, 40);
                } else {
                    if (preamp) smooth(preamp.gain, preampValue());
                    if (vocal) smooth(vocal.gain, voiceDb());
                }
            },
            release() { if (rebuildTimer) clearTimeout(rebuildTimer); clearGraph(); output.disconnect(); }
        };
        topology = key();
        try { build(); } catch (e) { clearGraph(); sourceNode.connect(output); }
        attachedElements.set(el, controller);
    }

    function registerElementEvents(el) {
        if (el.__ultraLightEventsAttached) return;
        el.__ultraLightEventsAttached = true;
        el.addEventListener("play", () => { wakeAudioContext(); trySetupSurround(el); }, { passive: true });
        el.addEventListener("playing", () => { wakeAudioContext(); trySetupSurround(el); }, { passive: true });
        el.addEventListener("pause", scheduleIdleSuspend, { passive: true });
        el.addEventListener("ended", scheduleIdleSuspend, { passive: true });
        el.addEventListener("emptied", scheduleIdleSuspend, { passive: true });
    }
    function scanMedia() {
        try { document.querySelectorAll("video, audio").forEach(el => { registerElementEvents(el); if (needsProcessing() && !el.paused) trySetupSurround(el); }); } catch (e) {}
    }
    let scanTimer = null;
    function scheduleScan() { if (scanTimer) return; scanTimer = setTimeout(() => { scanTimer = null; scanMedia(); }, 200); }
    const observer = new MutationObserver(muts => {
        for (const m of muts) {
            for (const node of m.addedNodes) {
                if (node.nodeType === 1 && (node.tagName === "VIDEO" || node.tagName === "AUDIO" || (node.querySelector && node.querySelector("video, audio")))) {
                    scheduleScan(); return;
                }
            }
        }
    });
    const root = document.documentElement || document.body;
    if (root) { try { observer.observe(root, { childList: true, subtree: true }); } catch (e) {} scanMedia(); }
    else { document.addEventListener("DOMContentLoaded", () => { const r = document.documentElement || document.body; if (r) try { observer.observe(r, { childList: true, subtree: true }); } catch (e) {} scanMedia(); }, { once: true }); }
    ["click", "pointerdown", "keydown"].forEach(evt => window.addEventListener(evt, () => { wakeAudioContext(); scanMedia(); }, { passive: true }));

    window.__UltraLightSurround = {
        updateConfig(newCfg) {
            cfg = Object.assign(cfg, newCfg);
            try {
                document.querySelectorAll("video, audio").forEach(el => {
                    const c = attachedElements.get(el);
                    if (c) c.update(cfg);
                    else if (needsProcessing() && !el.paused && canProcessElement(el)) trySetupSurround(el);
                });
            } catch (e) {}
        }
    };
"#;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generates_audio_dsp_and_update_scripts() {
        let mut settings = AppSettings::default();
        settings.system_audio_passthrough = false;
        let js = build_audio_dsp_script(&settings, true);
        assert!(js.contains("ultralight-true-peak-limiter"));
        assert!(js.contains("\"device\":\"headphones\""));

        settings.enable_night_mode = true;
        let upd = build_audio_update_script(&settings, false);
        assert!(upd.contains("\"nightMode\":true"));
        assert!(upd.contains("\"device\":\"speakers\""));
    }
}
