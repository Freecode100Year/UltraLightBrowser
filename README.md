# UltraLightBrowser 🚀

> **Ultra-Fast, Minimal, and Hardware-Optimized Windows 11 Native Browser Shell**  
> Powered by Microsoft Edge WebView2 Evergreen Runtime & Modern C++20.

[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20(x64)-0078d4.svg?logo=windows)](https://microsoft.com)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg?logo=c%2B%2B)](https://isocpp.org)
[![Build](https://img.shields.io/badge/Build-CMake%20%7C%20MSVC%202022-brightgreen.svg?logo=cmake)](https://cmake.org)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

---

## 📖 Overview

**UltraLightBrowser** is an engineered, bloatware-free Windows 11 desktop browser designed for extreme responsiveness, minimal memory footprint, and full hardware acceleration.

Unlike typical Electron or monolithic Chromium browsers that consume hundreds of megabytes at rest, UltraLightBrowser builds directly upon Windows 11 native Win32 APIs, Per-Monitor V2 High-DPI scaling, and the system-installed Microsoft Edge WebView2 Evergreen Runtime.

---

## ⚡ Key Highlights & Benchmarks

| Metric | Target / Measured | Technical Driver |
| :--- | :--- | :--- |
| **Binary Footprint** | `<= 2.0 MB` (Single `.exe`) | Zero-bloat Win32, MSVC `/O2 /GL /LTCG /OPT:REF /OPT:ICF` |
| **Cold Startup Time** | `<= 0.3s` | Native Win32 window loop, async WebView2 environment spin-up |
| **Idle / Minimized RAM** | `~20 MB` | `EmptyWorkingSet` & `ICoreWebView2_3::TrySuspend` on minimize |
| **Display Scaling** | Crisp 4K/8K HiDPI | `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` |
| **Visual Style** | Native Windows 11 | Mica/Immersive Dark Mode (`DWMWA_USE_IMMERSIVE_DARK_MODE`) & Rounded Corners |

---

## 🛠️ Architecture & Directory Structure

```text
UltraLightBrowser/
├── CMakeLists.txt              # MSVC 2022 & C++20 maximum optimization build definition
├── vcpkg.json                  # Modern C++ package manager dependencies (WIL, nlohmann-json)
├── packages.config             # NuGet alternative dependency manifest
├── LICENSE                     # MIT License
├── README.md                   # Complete architectural and usage guide
├── resources/
│   ├── app.manifest            # Per-Monitor V2 DPI, UTF-8 code page & Windows 11 OS compatibility
│   └── resource.rc             # Win32 resource definitions
└── src/
    ├── main.cpp                # Win32 wWinMain, High-DPI initialization, and message pump
    ├── MainWindow.hpp/.cpp     # Native Win32 dark frame, Segoe UI toolbar & accelerator dispatch
    ├── WebViewManager.hpp/.cpp # WebView2 Evergreen composition, GPU flags & lifecycle
    ├── ExtensionManager.hpp/.cpp# CRX3 in-memory unpacker, Chrome MV3 loading & popups
    ├── ElementBlocker.hpp/.cpp # Zero-flicker pre-render CSS injection & interactive DOM picker
    ├── PowerManager.hpp/.cpp   # EcoQoS suppression, thread priority elevation & memory trimming
    └── Config.hpp/.cpp         # Thread-safe JSON persistence for blocklist & settings
```

---

## 🚀 Core Features

### 1. 🏎️ Extreme Hardware Acceleration & Network Optimization
Through `WebViewManager`, the browser injects deep Chromium performance arguments on environment creation:
* **GPU Rasterization & Zero-Copy**: `--enable-gpu-rasterization --enable-zero-copy --enable-accelerated-video-decode`
* **DirectComposition Overlays**: `--enable-hardware-overlays=single-fullscreen,single-on-top --disable-direct-composition-video-overlays=false`
* **AI Super Resolution**: `--enable-features=NvidiaVsr,IntelVsr,Prerender2`
* **Low-Latency Transport**: `--enable-quic --quic-version=h3 --enable-bbr --enable-async-dns --enable-tcp-fast-open`
* **Bloatware Purge**: `--disable-features=Translate,OptimizationHints,MediaRouter --disable-background-networking --no-first-run`

### 2. 🧩 Chrome Extension (MV3) Support
`ExtensionManager` provides native Chrome extension capabilities via `ICoreWebView2Profile7`:
* **CRX3 Parser**: Validates `Cr24` magic headers, extracts format version 3 headers, and decompresses inner ZIP payloads into `%LOCALAPPDATA%\UltraLightBrowser\Extensions\<ID>`.
* **Manifest V3 Reader**: Parses `action.default_popup` and `action.default_icon`.
* **Borderless Win32 Popups**: Spawns isolated native popups for extension action pages on click.

### 3. 🛡️ Zero-Flicker Element Hiding & Interactive Picker
`ElementBlocker` ensures user privacy and ad-blocking without layout shifts:
* **Pre-Render Injection**: Injects domain-matched CSS rules via `AddScriptToExecuteOnDocumentCreated` before DOM construction, preventing ad flash.
* **Interactive DOM Picker (`Ctrl + Shift + H`)**: Injects an element inspector with red outlines; clicking an element computes its optimal CSS selector and saves it directly to local JSON storage.

### 4. 🔋 Power & WorkingSet Memory Optimization
`PowerManager` dynamically manages system and child processes:
* **EcoQoS Disabling**: Traverses process trees to disable `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`, ensuring maximum frame rates and zero micro-stutters during heavy media playback.
* **Smart Memory Trimming**: On `WM_SYSCOMMAND (SC_MINIMIZE)`, signals `ICoreWebView2_3::TrySuspend` and invokes `SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1)` (`EmptyWorkingSet`), purging unneeded physical pages down to ~20MB.
* **Instant Resume**: Restores execution immediately upon `SC_RESTORE` or focus.

---

## 💻 Building from Source

### Prerequisites
* **Operating System**: Windows 11 (x64)
* **Compiler**: Microsoft Visual Studio 2022 (with *Desktop development with C++*)
* **Build System**: CMake `>= 3.25`
* **Package Manager**: [vcpkg](https://github.com/microsoft/vcpkg) or NuGet
* **Runtime**: Microsoft Edge WebView2 Evergreen Runtime (pre-installed on Windows 11)

### Option A: Build with vcpkg (Recommended)

```powershell
# Clone the repository
git clone https://github.com/Freecode100Year/UltraLightBrowser.git
cd UltraLightBrowser

# Configure with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build optimized Release binary
cmake --build build --config Release

# Output executable:
# .\build\Release\UltraLightBrowser.exe
```

### Option B: Build with NuGet

```powershell
# Restore NuGet packages
nuget restore packages.config -PackagesDirectory packages

# Configure with CMake
cmake -B build -S .

# Build
cmake --build build --config Release
```

---

## ⌨️ Shortcuts

* <kbd>Ctrl</kbd> + <kbd>L</kbd> : Focus Address Bar
* <kbd>Ctrl</kbd> + <kbd>R</kbd> / <kbd>F5</kbd> : Reload Current Page
* <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd> : Toggle Interactive Element Hiding / Picker Mode
* <kbd>Alt</kbd> + <kbd>←</kbd> : Navigate Back
* <kbd>Alt</kbd> + <kbd>→</kbd> : Navigate Forward

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
