# Session Fingerprint Profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing macOS Edge identity mode use one cryptographically selected, coherent device profile per process session, shared by every tab and worker.

**Architecture:** Add a deterministic profile catalog testable without Windows APIs. Select one entry once per process using `BCryptGenRandom`, inject its paired GPU label and CPU concurrency into the existing document/worker stealth script, and extend the local identity page to show native rendering limitations. Leave Canvas/WebGL pixels, audio samples, network behavior, and the default Windows identity unchanged.

**Tech Stack:** C++20, WebView2 DevTools Protocol, Windows BCrypt RNG, existing embedded HTML/JS and g++ test harness.

**Spec:** `docs/superpowers/specs/2026-11-10-session-fingerprint-design.md`

## Global Constraints

- One process session uses one profile across regular/private windows, tabs, iframes, and supported workers.
- Profile selection uses Windows `BCryptGenRandom`; no weak RNG fallback and no persistent seed/profile.
- UA and Edge/Chromium versions remain derived from the actual WebView2 runtime.
- Canvas/WebGL pixel output, audio readback, and network fingerprint remain native and are described as uncovered.
- The default Windows identity does not change.
- The four profile tuples are Apple M1/8, M2/8, M3/8, and M4/10 hardware-concurrency values; repeats between restarts are allowed.

## Review Focus

- RNG failure uses the fixed coherent M1 profile, never weak randomness; test selection fallback.
- All WebViewManager instances share one selected profile; test the process-static contract and self-test fields.
- Worker and document fields agree; test generated script substitutions and worker probe rows.
- Native rendering is not reported as randomized; test self-test labels and unchanged pixel/audio APIs.
- Profile collisions across restarts are possible; user-facing text must not promise every restart differs; check docs/copy.

---

### Task 1: Coherent profile catalog and script generation

**Files:**
- Create: `src/MacDeviceProfile.hpp`
- Modify: `src/MacStealth.hpp`
- Create: `tests/mac_device_profile_test.cpp`
- Modify: `tests/README.md`

**Interfaces:**
- Produces `UltraLight::MacDeviceProfile` with `std::string_view renderer` and `unsigned hardwareConcurrency`.
- Produces `const MacDeviceProfile& MacDeviceProfileForIndex(std::size_t index)`, `std::size_t MacDeviceProfileCount()`, and `std::size_t MacDeviceProfileIndex(bool rngSucceeded, std::uint32_t randomValue)`; failed RNG returns index zero.
- Produces `std::wstring BuildMacStealthScript(const MacDeviceProfile& profile)`; it replaces `__ULB_MAC_GPU_RENDERER__` and `__ULB_MAC_HARDWARE_CONCURRENCY__` in the stealth template.

- [ ] **Step 1: Write failing profile and script tests**

Assert the four expected profile pairs, index wraparound, failed-RNG index zero, stable references, no unresolved placeholders after generation, the chosen renderer and core count are present, and Canvas/audio pixel APIs are not patched by the script.

- [ ] **Step 2: Run the profile test and confirm it fails to compile**

Run: `g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/mac_device_profile_test.cpp -o /tmp/mac-device-profile-tests && /tmp/mac-device-profile-tests`
Expected: compile failure because the profile header/accessors are not implemented.

- [ ] **Step 3: Implement profile catalog and script generator**

Add the fixed four-entry profile table and pure placeholder substitution. The script uses the profile's WebGL renderer and overrides `Navigator.hardwareConcurrency` and `WorkerNavigator.hardwareConcurrency`; other API readbacks remain untouched.

- [ ] **Step 4: Run the profile test**

Run the command above. Expected: PASS.

- [ ] **Step 5: Document the test command**

Add the exact command and coverage to `tests/README.md`.

### Task 2: Process-scoped secure profile selection and WebView2 injection

**Files:**
- Modify: `src/WebViewManager.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/mac_device_profile_test.cpp`

**Interfaces:**
- Consumes: profile catalog and `BuildMacStealthScript` from Task 1.
- Produces: a function-local process-wide profile selection initialized using `BCryptGenRandom`; on failure it selects catalog index zero through the already-tested pure index selector.

- [ ] **Step 1: Confirm the profile-index selector test covers RNG failure behavior**

The Task 1 test asserts failed RNG maps to profile index zero and successful values map into the catalog. This isolates deterministic selection policy from the Windows BCrypt API. The generated-script test uses one representative profile; the catalog test covers all four profile tuples.

- [ ] **Step 2: Add process-wide selection and use the generated source**

Use `BCryptGenRandom(nullptr, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)` to obtain a random catalog index; if it fails, select index zero. Add `bcrypt.lib` to linked libraries. Build the per-WebView script from the shared selected profile, then retain the existing document, iframe, Dedicated Worker, and Service Worker injection lifecycle.

- [ ] **Step 3: Run profile and UA tests**

Run the profile test and `g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/user_agent_test.cpp -o /tmp/ua-tests && /tmp/ua-tests`. Expected: PASS.

### Task 3: Self-test coverage and limitations

**Files:**
- Modify: `src/SelfTestPage.hpp`
- Modify: `tests/ui_resources_test.py` only if assertions are needed

- [ ] **Step 1: Add failing self-test coverage assertions**

Check that the self-test includes `navigator.hardwareConcurrency`, compares it in a Dedicated Worker, and states Canvas/WebGL pixel output and audio readback are native/not randomized.

- [ ] **Step 2: Run the UI resource test and confirm failure**

Run: `python3 tests/ui_resources_test.py`
Expected: FAIL until the self-test text and worker check are updated.

- [ ] **Step 3: Update the embedded self-test**

Display selected GPU label and concurrency, verify document/worker equality, and note that actual pixel rendering/audio and host fonts/GPU may still identify the host. Preserve the secure-origin worker probes. Do not promise every restart changes the profile.

- [ ] **Step 4: Run UI resource checks**

Run: `python3 tests/ui_resources_test.py`. Expected: PASS.

### Task 4: Verification and Windows executable

**Files:**
- Modify: `README.md` and `FULL_VERSION_GUIDE.md` only if current claims become inaccurate.
- Modify: `.github/workflows/validate.yml` to retain the Windows Release exe as a CI artifact.

- [ ] **Step 1: Run portable regression checks**

Run profile test, UA test, UI resource test, and the relevant existing validation commands. Expected: all PASS.

- [ ] **Step 2: Build Release executable on Windows**

Run: `./build-windows.ps1 -Configuration Release`. Expected: non-empty `dist/Release/UltraLightBrowser.exe`.

- [ ] **Step 3: Download and verify the Windows Release artifact from the feature branch CI run**

When the local host cannot run the Windows build script, push the authorized feature branch, wait for its existing Windows build job, download the Release exe artifact, and verify the file exists and is non-empty. Do not publish a GitHub Release.
