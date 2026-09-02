# DKChatebras

DKChatebras is a fast, local Cerebras chat client with native Android, Windows, and Fedora GNOME editions plus the original dependency-light web app.

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

## Fedora GNOME / AppImage

`linux/` contains source for a fully native C17 GTK4/libadwaita application targeting current Fedora. It uses libsoup 3, json-glib, and GNOME Secret Service; it does not use Electron or a WebView.

- Appearance: current adaptive GNOME/libadwaita design
- API key: GNOME Keyring/Secret Service
- History: metadata index plus separate per-conversation JSON files
- Packaging: Meson project and `linuxdeploy` AppImage script

Build on current Fedora from `linux/`:

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config \
  gtk4-devel libadwaita-devel libsoup3-devel \
  json-glib-devel libsecret-devel
meson setup build --buildtype=release
meson compile -C build
./build/src/dkchatebras
```

Generate an AppImage on Fedora:

```sh
chmod +x packaging/build-appimage.sh
./packaging/build-appimage.sh
```

The Linux edition is intentionally not compiled on this Windows host. See [`linux/README.md`](linux/README.md) for full dependency, installation, AppImage, and validation instructions.

## Privacy

Requests go directly from each client to `https://api.cerebras.ai/v1/chat/completions`. Conversation content remains local until included as context in a Cerebras request. Native editions protect API keys with their platform credential facilities; conversation text itself is not encrypted.
