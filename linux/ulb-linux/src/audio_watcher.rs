use pipewire as pw;
use std::collections::HashMap;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use ulb_core::audio_endpoint::is_headphone_endpoint;

#[derive(Debug, Clone)]
struct SinkInfo {
    name: String,
    description: String,
    form_factor: String,
}

fn eval_headphones(sinks: &HashMap<u32, SinkInfo>) -> bool {
    sinks.values().any(|s| {
        let hint = s.form_factor.eq_ignore_ascii_case("headphone")
            || s.form_factor.eq_ignore_ascii_case("headset");
        is_headphone_endpoint(hint, &s.name, &s.description)
    })
}

/// Watches PipeWire audio sink nodes for headphone plug/unplug events (debounced 250ms).
pub struct AudioWatcher {
    stop_flag: Arc<AtomicBool>,
    headphones_active: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl AudioWatcher {
    pub fn start<F>(on_change: F) -> Self
    where
        F: Fn(bool) + Send + Sync + 'static,
    {
        let stop_flag = Arc::new(AtomicBool::new(false));
        let headphones_active = Arc::new(AtomicBool::new(false));
        let stop_bg = Arc::clone(&stop_flag);
        let hp_bg = Arc::clone(&headphones_active);

        let thread = thread::spawn(move || {
            pw::init();
            let Ok(mainloop) = pw::main_loop::MainLoopBox::new(None) else {
                return;
            };
            let Ok(context) = pw::context::ContextBox::new(mainloop.loop_(), None) else {
                return;
            };
            let Ok(core) = context.connect(None) else {
                return;
            };
            let Ok(registry) = core.get_registry() else {
                return;
            };

            let sinks: Arc<Mutex<HashMap<u32, SinkInfo>>> = Arc::new(Mutex::new(HashMap::new()));
            let dirty_since: Arc<Mutex<Option<Instant>>> = Arc::new(Mutex::new(None));

            let sinks_add = Arc::clone(&sinks);
            let dirty_add = Arc::clone(&dirty_since);
            let sinks_rm = Arc::clone(&sinks);
            let dirty_rm = Arc::clone(&dirty_since);

            let _listener = registry
                .add_listener_local()
                .global(move |global| {
                    if global.type_ != pw::types::ObjectType::Node {
                        return;
                    }
                    let Some(props) = global.props else {
                        return;
                    };
                    if props.get("media.class") != Some("Audio/Sink") {
                        return;
                    }
                    let info = SinkInfo {
                        name: props.get("node.name").unwrap_or_default().to_string(),
                        description: props
                            .get("node.description")
                            .or_else(|| props.get("device.description"))
                            .unwrap_or_default()
                            .to_string(),
                        form_factor: props
                            .get("device.form-factor")
                            .or_else(|| props.get("device.icon-name"))
                            .unwrap_or_default()
                            .to_string(),
                    };
                    sinks_add.lock().unwrap().insert(global.id, info);
                    *dirty_add.lock().unwrap() = Some(Instant::now());
                })
                .global_remove(move |id| {
                    if sinks_rm.lock().unwrap().remove(&id).is_some() {
                        *dirty_rm.lock().unwrap() = Some(Instant::now());
                    }
                })
                .register();

            let pw_loop = mainloop.loop_();
            while !stop_bg.load(Ordering::Relaxed) {
                pw_loop.iterate(pipewire::loop_::Timeout::Finite(Duration::from_millis(120)));
                let should_emit = {
                    let mut d = dirty_since.lock().unwrap();
                    if let Some(ts) = *d {
                        if ts.elapsed() >= Duration::from_millis(250) {
                            *d = None;
                            true
                        } else {
                            false
                        }
                    } else {
                        false
                    }
                };
                if should_emit {
                    let active = eval_headphones(&sinks.lock().unwrap());
                    let prev = hp_bg.swap(active, Ordering::Relaxed);
                    if active != prev {
                        on_change(active);
                    }
                }
            }
        });

        Self {
            stop_flag,
            headphones_active,
            thread: Some(thread),
        }
    }

    pub fn is_headphones_active(&self) -> bool {
        self.headphones_active.load(Ordering::Relaxed)
    }

    pub fn stop(&mut self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        if let Some(h) = self.thread.take() {
            let _ = h.join();
        }
    }
}

impl Drop for AudioWatcher {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn evaluates_sink_map_for_headphones() {
        let mut map = HashMap::new();
        map.insert(
            1,
            SinkInfo {
                name: "alsa_output.pci-0000_00_1f.3.analog-stereo".to_string(),
                description: "Built-in Audio Analog Stereo Speakers".to_string(),
                form_factor: "speaker".to_string(),
            },
        );
        assert!(!eval_headphones(&map));

        map.insert(
            2,
            SinkInfo {
                name: "bluez_output.AA_BB_CC.a2dp-sink".to_string(),
                description: "WH-1000XM5 Headphones".to_string(),
                form_factor: "headphone".to_string(),
            },
        );
        assert!(eval_headphones(&map));
    }
}
