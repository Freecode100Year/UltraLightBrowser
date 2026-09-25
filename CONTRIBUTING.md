# Contributing to UltraLightBrowser

Thank you for your interest in contributing to **UltraLightBrowser**! 🚀

## Core Principles
1. **Zero Bloat**: We strictly prefer native Win32, C++20, and Microsoft Edge WebView2 Evergreen runtime over heavy third-party GUI frameworks or Chromium source modifications.
2. **Speed & Efficiency**: Cold start should remain <= 0.3s, and idle memory <= 20MB.
3. **Safety & Robustness**: Modern RAII (via WIL), no raw pointer leaks, and defense-in-depth security.

## Development Workflow
1. Fork the repository and create your feature branch:
   ```bash
   git checkout -b feature/my-new-feature
   ```
2. Build and verify locally on Windows 11 / Windows 10:
   ```powershell
   cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```
3. Run the binary in `build\Release\UltraLightBrowser.exe` and test your changes.
4. Commit your changes with conventional commit messages (`feat: ...`, `fix: ...`, `docs: ...`).
5. Open a Pull Request!

## Star & Share
Even if you aren't writing code, starring the repo ⭐, opening issues, or sharing feedback helps the project tremendously!
