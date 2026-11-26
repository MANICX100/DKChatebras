# DKChatebras

[![Latest Release](https://img.shields.io/github/v/release/MANICX100/DKChatebras?style=flat-square&color=ff5b38&label=release)](https://github.com/MANICX100/DKChatebras/releases/latest)
[![Release Date](https://img.shields.io/github/release-date/MANICX100/DKChatebras?style=flat-square&color=blue)](https://github.com/MANICX100/DKChatebras/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/MANICX100/DKChatebras/total?style=flat-square&color=brightgreen)](https://github.com/MANICX100/DKChatebras/releases)
[![Web Deploy](https://img.shields.io/github/actions/workflow/status/MANICX100/DKChatebras/pages.yml?style=flat-square&label=web%20deploy)](https://github.com/MANICX100/DKChatebras/actions/workflows/pages.yml)
[![Live Web App](https://img.shields.io/badge/web%20app-live-ff5b38?style=flat-square&logo=githubpages&logoColor=white)](https://manicx100.github.io/DKChatebras/)
[![Last Commit](https://img.shields.io/github/last-commit/MANICX100/DKChatebras?style=flat-square)](https://github.com/MANICX100/DKChatebras/commits/main)
[![Repo Size](https://img.shields.io/github/repo-size/MANICX100/DKChatebras?style=flat-square)](https://github.com/MANICX100/DKChatebras)
[![Stars](https://img.shields.io/github/stars/MANICX100/DKChatebras?style=flat-square)](https://github.com/MANICX100/DKChatebras/stargazers)
[![Issues](https://img.shields.io/github/issues/MANICX100/DKChatebras?style=flat-square)](https://github.com/MANICX100/DKChatebras/issues)

[![Linux](https://img.shields.io/badge/Linux-C17%20%7C%20X11%20%2B%20FreeType2-FCC624?style=flat-square&logo=linux&logoColor=black)](linux/README.md)
[![Windows](https://img.shields.io/badge/Windows-C%2B%2B17%20%7C%20Win32-0078D6?style=flat-square&logo=windows&logoColor=white)](windows/README.md)
[![Scoop](https://img.shields.io/badge/Scoop-dkchatebras-0078D6?style=flat-square&logo=windows&logoColor=white)](bucket/dkchatebras.json)
[![Android](https://img.shields.io/badge/Android-Java%20%7C%20API%2023--37-3DDC84?style=flat-square&logo=android&logoColor=black)](android/README.md)
[![Web](https://img.shields.io/badge/Web-Vanilla%20JS%20%2F%20HTML%20%2F%20CSS-F7DF1E?style=flat-square&logo=javascript&logoColor=black)](https://manicx100.github.io/DKChatebras/)
[![Linux License](https://img.shields.io/badge/Linux%20license-GPL--3.0--or--later-blue?style=flat-square&logo=gnu&logoColor=white)](linux/COPYING)

[![Linux Binary](https://img.shields.io/badge/Linux%20binary-176%20KiB-2ea44f?style=flat-square)](linux/README.md#benchmarks-vs-previous-gtk4libadwaita-build-v3)
[![Linux Startup](https://img.shields.io/badge/Linux%20startup-35.6%20ms-2ea44f?style=flat-square)](linux/README.md#benchmarks-vs-previous-gtk4libadwaita-build-v3)
[![Linux Idle Memory](https://img.shields.io/badge/Linux%20idle%20PSS-4.6%20MiB-2ea44f?style=flat-square)](linux/README.md#benchmarks-vs-previous-gtk4libadwaita-build-v3)
[![Linux Direct Libs](https://img.shields.io/badge/Linux%20direct%20libs-6-2ea44f?style=flat-square)](linux/README.md#benchmarks-vs-previous-gtk4libadwaita-build-v3)
[![Markdown Parse](https://img.shields.io/badge/Markdown%20parse-160%20MB%2Fs-2ea44f?style=flat-square)](linux/README.md#benchmarks-vs-previous-gtk4libadwaita-build-v3)
[![No Electron or WebView](https://img.shields.io/badge/Electron%20%2F%20WebView-none-ff5b38?style=flat-square)](#dkchatebras)
[![Cerebras API](https://img.shields.io/badge/API-Cerebras%20SSE-7c3aed?style=flat-square)](https://cerebras.ai/)
[![Models](https://img.shields.io/badge/Models-gpt--oss--120b%20%7C%20gemma--4--31b-555555?style=flat-square)](#dkchatebras)

DKChatebras is a fast, local Cerebras chat client with native Android, Windows, and Linux editions plus the original dependency-light web app.

All editions support:

- Cerebras streaming chat completions
- `gpt-oss-120b` and `gemma-4-31b`
- Locally stored API-key management
- Lazy conversation history: metadata loads at startup; messages load only when selected
- Plain-text streaming followed by one-pass native or sanitized HTML-style Markdown formatting
- New, delete, and cancellable conversations

## Before You Start: Get a Cerebras API Key (Required)

> [!IMPORTANT]
> **A Cerebras API key is required to use DKChatebras.**
> DKChatebras is a free client app — it does **not** include built-in AI credits or a subscription. Your messages go directly from your device to Cerebras, so you need your own Cerebras Cloud account and API key before the app can answer prompts.

Never used an API key before? Follow these 4 quick steps:

### 1. Create a Cerebras Cloud account
1. Open [**cloud.cerebras.ai**](https://cloud.cerebras.ai/) in your browser.
2. Click **Sign Up** and create an account (using email, Google, or GitHub).

### 2. Set up billing / add prepaid credits
Cerebras uses **prepaid pay-as-you-go credits** (similar to topping up a gift card — **not** a recurring monthly subscription):
1. In the Cerebras Cloud Console, open the **Billing** / **Pay as you go** tab.
2. **Add a payment card:** Adding a verified card unlocks a one-time **$5 promotional credit** (valid for 30 days) so you can try it out — you are not charged just for adding a card.
3. **Buy credits when ready:** Click **Add Credits** to top up your balance (deposits start at **$10 USD**). Buying credits upgrades your account to the **Developer** tier with higher rate limits and larger context windows.
4. **Stay in control of spending:**
   - **Auto-Recharge is OFF by default.** If your balance reaches \$0, requests simply stop until you choose to add more credits — no surprise bills.
   - You only pay for the exact tokens you send and receive. Check current per-model rates at [**cerebras.ai/pricing**](https://www.cerebras.ai/pricing) and monitor your spend anytime under **Usage Monitoring → Cost**.

### 3. Generate and copy your API key
1. In the left sidebar of [**cloud.cerebras.ai**](https://cloud.cerebras.ai/), click **API Keys** (if you are on a paid account, select a project first).
2. Click **Generate API Key**, type a label like `DKChatebras`, and confirm.
3. **Copy the key right away** (it looks like `csk-...`). For security, Cerebras will not display the full key again once you close the popup.
4. Treat your API key like a password — never post it publicly or commit it to Git.

### 4. Paste your key into DKChatebras
- **Linux:** Click the key icon in the top bar (or press `Ctrl+K`), paste your key, and click **Save Key** (or set the `CEREBRAS_API_KEY` environment variable).
- **Windows:** Open the API key settings in the app, paste your key, and save (encrypted with Windows DPAPI).
- **Android:** Tap **Key** in the top bar (or **Manage API key** in the drawer), paste your key, and save (encrypted in Android Keystore).
- **Web:** Paste your key directly into the top-bar API key field.

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

### Quick install via Scoop

Install directly from the Scoop manifest ([`bucket/dkchatebras.json`](bucket/dkchatebras.json)) in PowerShell:

```powershell
scoop install https://raw.githubusercontent.com/MANICX100/DKChatebras/main/bucket/dkchatebras.json
```

Or add this repository as a Scoop bucket for easy `scoop update dkchatebras` updates:

```powershell
scoop bucket add dkchatebras https://github.com/MANICX100/DKChatebras && scoop install dkchatebras/dkchatebras
```

*(Don't have [Scoop](https://scoop.sh/) yet? Install it first in PowerShell with `irm get.scoop.sh | iex`.)*

### Build from source

From PowerShell in `windows/`:

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

### Quick install (x86_64)

Privileged commands below use [`doas`](https://wiki.archlinux.org/title/Doas) ([OpenDoas install & setup guide](https://github.com/Duncaen/OpenDoas)) by default, with `sudo` alternatives provided.

Install directly into `/usr/local/bin/dkchatebras` with `doas`:

```sh
curl -fsSL https://github.com/MANICX100/DKChatebras/releases/download/v4/DKChatebras-v4-linux-x86_64 -o /tmp/dkchatebras && doas install -Dm755 /tmp/dkchatebras /usr/local/bin/dkchatebras && rm -f /tmp/dkchatebras
```

Or with `sudo`:

```sh
curl -fsSL https://github.com/MANICX100/DKChatebras/releases/download/v4/DKChatebras-v4-linux-x86_64 -o /tmp/dkchatebras && sudo install -Dm755 /tmp/dkchatebras /usr/local/bin/dkchatebras && rm -f /tmp/dkchatebras
```

### Build from source

From `linux/`:

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
