# Power manager regression tests

Compile and run on Linux with a C++20 compiler:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Itests/stubs -Isrc tests/power_manager_test.cpp src/PowerManager.cpp -o /tmp/power-tests
/tmp/power-tests
```

The tests compile the actual `src/PowerManager.cpp`. The headers in `stubs/`
provide deterministic Win32/WebView2 API boundaries so failures and callback
ordering can be reproduced without Windows. They are only on the test include
path and do not enter the application build. Assertions remain active in
optimized builds.

Coverage includes rejected and failed suspension, retry, foreground restore
racing suspension completion, background audio racing completion, controller
visibility, memory target restoration, audio renderer throttling, descendants,
unrelated PIDs, duplicate/cyclic entries, and one snapshot per traversal.

These tests do not emulate real COM ownership, Windows scheduling, or WebView2
playback. The separate Windows CI job compiles and links the native application.
Before release, test on Windows 11 with WebView2:

1. Minimize a silent page, then restore it rapidly and after several seconds.
   Confirm the page stays visible and interactive.
2. Play audio, minimize, and confirm playback continues. Stop and restart audio
   in the background (for example using media controls), then restore.
3. Leave a silent window unfocused for five minutes, then focus it again.
4. Repeat minimize/restore during navigation and with multiple renderer processes.
5. Verify child renderer EcoQoS is removed for foreground and audio playback.

## Fullscreen geometry

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/window_geometry_test.cpp -o /tmp/geometry-tests
/tmp/geometry-tests
```

Tests cover fullscreen from normal/maximized state, normal maximization respecting
the taskbar, and negative monitor coordinates. On Windows, check F11 and a video
player's fullscreen button from both normal and maximized windows. Confirm all
four edges cover the monitor, square corners, no taskbar gap, and correct restore
with Esc/F11. Repeat on a second display and at 125%/150% DPI.

## Audio routing

```sh
node tests/audio_enhancer_test.cjs
```

The test reconstructs the actual C++ raw-string injection script and executes it
with deterministic DOM/Web Audio boundaries. It verifies native mode creates no
AudioContext, speaker output uses separate merger channels, disabled effects
bypass processing, speaker mode avoids HRTF, dialogue uses mild compression,
diagnostics do not claim native media, and gain updates preserve the source.
These tests verify graph topology and parameters, not audible quality or actual
browser channel-mixing/latency behavior. Native Windows checks are required.

`python3 tests/audio_registration_test.py` compiles the actual C++ registration
block extracted from WebViewManager.cpp. Deterministic callbacks exercise both
completion orders, retained reload requests, stale-script removal, asynchronous
failure and synchronous failure. It still does not emulate real COM ownership.
