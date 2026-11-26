use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;
use ulb_core::warp_host::{public_line, run_parse_only, WarpConfig, WarpSupervisor};

#[test]
fn parses_and_supervises_real_ulb_warp_binary_when_available() {
    let Some(bin_str) = std::env::var_os("ULB_WARP_TEST_BIN") else {
        return;
    };
    let bin = PathBuf::from(bin_str);
    assert!(bin.exists(), "ULB_WARP_TEST_BIN does not exist: {:?}", bin);

    // 1. Verify -probe -parse-only against real Go parser.
    let pub_link = public_line();
    let checks = run_parse_only(&bin, &[pub_link.as_str(), "not-a-valid-link"], Duration::from_secs(5)).unwrap();
    assert_eq!(checks.len(), 2);
    assert!(checks[0].ok);
    assert!(!checks[1].ok);

    // 2. Supervise real ulb-warp child on 127.0.0.1:0 until PORT is printed, then cleanly stop.
    let state_dir =
        std::env::temp_dir().join(format!("ulb-warp-it-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&state_dir);

    let cfg = WarpConfig {
        binary: bin,
        state_dir: state_dir.clone(),
        fail_closed: true,
        use_line: true,
        selected_id: ulb_core::line_store::LineStore::PUBLIC.to_string(),
        selected_links: vec![],
    };

    let pair = Arc::new((Mutex::new(0u16), Condvar::new()));
    let pair_cb = Arc::clone(&pair);
    let mut sup = WarpSupervisor::start(&cfg, move |st| {
        if st.port != 0 {
            let (lock, cv) = &*pair_cb;
            let mut g = lock.lock().unwrap();
            *g = st.port;
            cv.notify_all();
        }
    })
    .unwrap();

    let (lock, cv) = &*pair;
    let port_guard = lock.lock().unwrap();
    let (guard, timeout) = cv
        .wait_timeout_while(port_guard, Duration::from_secs(5), |&mut p| p == 0)
        .unwrap();
    assert!(!timeout.timed_out(), "ulb-warp failed to report PORT in 5s");
    assert!(*guard > 0);
    drop(guard);

    sup.rescan();
    sup.stop();
    let _ = std::fs::remove_dir_all(&state_dir);
}
