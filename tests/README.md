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
