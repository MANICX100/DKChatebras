# DKChatebras

DKChatebras is a fast, local Cerebras chat client with native Android, Windows, and Linux editions plus the original dependency-light web app.

All editions support:

- Cerebras streaming chat completions
- `gpt-oss-120b` and `gemma-4-31b`
- Locally stored API-key management
- Lazy conversation history: metadata loads at startup; messages load only when selected
- Plain-text streaming followed by one-pass native or sanitized HTML-style Markdown formatting
- New, delete, and cancellable conversations

## Web

The root `index.html`, `app.js`, and `styles.css` implement the web edition.

```sh
python -m http.server 8080
```

Open `http://localhost:8080` and paste your Cerebras API key directly into the top-bar field. It saves automatically in browser `localStorage`; clearing the field removes it. Conversation summaries and messages use separate IndexedDB stores. A source key in `config.js` remains an optional migration fallback.

Anyone with access to the browser profile or developer tools can read a web-stored key. Use the web edition only on a trusted local profile.

### Web performance

- No framework runtime, font download, or image request
- Token paints batched to animation frames
- Markdown parser and sanitizer loaded only after first use
- Markdown parsed once after streaming
- Off-screen message rendering skipped with `content-visibility`

## Android

`android/` contains a fully native Java/Android Views application. It targets Android 17/API 37 and uses an expressive Material 3-inspired interface without Compose, AndroidX, a WebView, or runtime dependencies.

- Minimum: Android 6.0/API 23, the earliest practical target for native Android Keystore AES-GCM protection
- API key: device-bound Android Keystore encryption
- Streaming feedback: subtle system-controlled haptic ticks, throttled to 80ms
- History: private metadata index plus separate atomic JSON chat files
- Debug APK: `android/app/build/outputs/apk/debug/app-debug.apk`

Build from `android/`:

```sh
./gradlew assembleDebug
```

See [`android/README.md`](android/README.md) for installation and architecture details.

## Windows

`windows/` contains a fully native C++17 Win32 application using GDI/RichEdit, WinHTTP, and DPAPI. It has no .NET, WebView, package-manager, or third-party runtime requirement.

- Compatibility target: Windows 7 SP1 or later
- Appearance: Windows 11 25H2-inspired dark, rounded, Segoe UI interface with graceful legacy fallback
- API key: current-user Windows DPAPI encryption
- Input: focused and accent-highlighted by default
- Packaging: static MSVC runtime in one x64 executable
- Release executable: `windows/build/bin/DKChatebras.exe`

Build from PowerShell in `windows/`:

```powershell
.\build.ps1 -Configuration Release -Clean
```

Windows 7 requires current TLS 1.2 and root-certificate servicing to reach the Cerebras HTTPS endpoint. See [`windows/README.md`](windows/README.md) for details.

## Linux

`linux/` contains a minimal, performance-first native C17 Linux client using direct X11 (MIT-SHM), FreeType2, Fontconfig, and lazy-loaded system OpenSSL — with no Electron, WebView, GTK, Qt, or GLib runtime.

- Footprint: **~176 KiB** stripped standalone executable (6 direct shared libraries, ~35 ms startup, ~4.6 MiB PSS idle)
- Appearance: responsive dark interface with collapsible/overlay sidebar, bordered GFM tables, LaTeX math symbol normalization, and fenced code containers
- API key: `CEREBRAS_API_KEY` env var, Secret Service (`secret-tool`), or `0600` local config fallback
- History: metadata index plus separate per-conversation JSON files written asynchronously (100% compatible with previous Linux versions)

Build on Linux from `linux/`:

```sh
make release
make test
./build/dkchatebras
```

Build standalone release binary and portable `.tar.gz` bundle:

```sh
./packaging/build-release.sh v4
```

See [`linux/README.md`](linux/README.md) for build dependencies, keyboard shortcuts, installation, and benchmarks against the previous GTK4 edition.

## Privacy

Requests go directly from each client to `https://api.cerebras.ai/v1/chat/completions`. Conversation content remains local until included as context in a Cerebras request. Native editions protect API keys with their platform credential facilities; conversation text itself is not encrypted.
