# DKChatebras for Android

DKChatebras is a native Java/Android Views client for Cerebras Chat Completions. It uses no WebView, Compose, AndroidX, or third-party runtime libraries.

## Requirements

- Android Studio / JDK 17 or newer
- Android SDK Platform 37 and Build Tools 37
- Internet access on the first build so Gradle can obtain AGP 9.3.1

The app compiles and targets API 37 (Android 17). `minSdk 23` is the earliest practical baseline because API 23 introduced the Android Keystore AES key-generation APIs and AES-GCM parameter support used to protect the API key. Supporting older releases would require a weaker fallback, a substantially more complex hybrid encryption scheme, or another security dependency. API 23 also retains broad device coverage while allowing the project to remain dependency-light.

## Build and install

From this `android/` directory:

```sh
./gradlew assembleDebug
./gradlew installDebug
```

On Windows Command Prompt or PowerShell:

```bat
gradlew.bat assembleDebug
gradlew.bat installDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`. To install directly with ADB:

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Usage

1. Open DKChatebras and enter a Cerebras API key. It can later be updated or removed from **Key** in the app bar or **Manage API key** in the drawer.
2. Choose `gpt-oss-120b` or `gemma-4-31b` from the model button.
3. Send a prompt. Tokens appear as plain text with subtle, system-controlled haptic ticks while streaming; Markdown is rendered once generation finishes.
4. Open the navigation drawer to create, load, or permanently delete conversations. Tap the red stop control to cancel an active request.

## Architecture

- `MainActivity` builds the Material 3 Expressive-inspired interface entirely with native Views and `GradientDrawable` shapes. It owns chat, drawer, model/key dialogs, scrolling, and UI state.
- `CerebrasClient` performs a direct HTTPS `POST` to `https://api.cerebras.ai/v1/chat/completions` with `HttpURLConnection`. It parses streaming SSE `data:` records on a single background executor and supports cancellation by disconnecting the active request.
- `SecureApiKey` generates a non-exportable 256-bit AES key in `AndroidKeyStore` and stores only AES-GCM IV/ciphertext in private preferences. Backups are disabled to avoid restoring ciphertext without its device-bound key.
- `ConversationStore` keeps `files/conversations/index.json` as lightweight metadata. Each conversation has a separate UUID-named JSON file; messages are read only when that conversation is selected. Writes use a temporary file, `fsync`, and rename.
- `MarkdownRenderer` converts headings, nested emphasis, strikethrough, inline/fenced code, blockquotes, ordered/unordered lists, links, horizontal rules, and escapes to native Android spans. Raw HTML remains visible text; no HTML parser or WebView is involved.

The app requests only `INTERNET`, rejects cleartext network traffic, and sends the API key only as the Cerebras authorization header.
