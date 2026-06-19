# CLAUDE.md

Guidance for coding agents working in this repository.

## What is LeanCast?

LeanCast is a Windows-only native C++ app launcher. It opens a compact Raycast/Spotlight-style overlay from a global shortcut, searches installed apps and open windows, and launches or focuses the selected result.

AI chat and all AI provider settings have been removed. Do not reintroduce AI UI, API key storage, or network AI calls unless explicitly requested.

## Tech Stack

- C++23
- Win32 message loop, tray icon, shell APIs, low-level keyboard hook
- Direct2D + DirectWrite rendering
- WIC PNG icon cache
- CMake/CPack build

## Architecture

```
CMakeLists.txt              # Native build, tests, CPack packaging
native/
  LeanCast.rc.in           # CMake-generated icon and manifest resource template
  app.manifest             # DPI/common-controls manifest
  src/core.hpp             # Pure fuzzy search core used by app and tests
  src/main.cpp             # Win32 app, UI, discovery, settings, icons, tray
  tests/core_tests.cpp     # Core fuzzy/search tests
scripts/gen-icons.ps1      # Generates build/icon.ico, icon.png, tray.png
build/                     # Generated icon assets used by native resources
```

## Behavior Notes

- Settings are stored in `%APPDATA%\LeanCast\settings.json`. Only native-supported fields are saved: `shortcut`, `recentApps`, `compactMode`, `syncAccentColor`, and `customAccentColor`.
- App discovery scans Start Menu `.lnk` files and AppsFolder shell entries, then de-duplicates by id/name.
- Icons are lazy-loaded from the Windows Shell and cached as PNG files under `%APPDATA%\LeanCast\icon-cache-native`.
- The low-level keyboard hook handles both global shortcut monitoring and settings shortcut recording.
- The app is single-instance via a named mutex; a second launch asks the existing window to open search.

## Commands

```powershell
cmake -S . -B build-native -G "Visual Studio 17 2022" -A x64
cmake --build build-native --config Release
ctest --test-dir build-native -C Release
cpack --config build-native/CPackConfig.cmake -C Release
```

## Conventions

- Keep UI text and comments in English.
- Keep the app Windows-native; do not add Electron, WebView, Qt, or Node runtime dependencies without explicit approval.
- Regenerate app icons through `scripts/gen-icons.ps1`; do not hand-edit generated icon files.
