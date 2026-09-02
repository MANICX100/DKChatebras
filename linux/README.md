# DKChatebras for Fedora GNOME

DKChatebras is a fully native GNOME desktop client written in C. It uses GTK4 and libadwaita for the adaptive interface, libsoup 3 for direct streaming Cerebras API requests, json-glib for request/history JSON, and libsecret for GNOME Secret Service integration. It does not use Electron, a browser engine, or a WebView.

This project targets **current Fedora only**. The model IDs in the selector are normal application data and can be updated in `src/dk-window.c` when Cerebras changes its currently offered models.

## Features

- Adaptive libadwaita overlay sidebar for narrow and wide windows
- Lazy history loading: startup reads metadata only; a chat file is loaded when its row is activated
- Separate JSON file per chat, plus a small metadata index
- New chat, delete chat, stop generation, model selection, and automatic scrolling
- Direct `POST https://api.cerebras.ai/v1/chat/completions` with streaming SSE
- API key stored by Secret Service (normally GNOME Keyring), never in app configuration or history
- Native `GtkTextView` rendering with tags for headings 1–6, nested emphasis, strikethrough, inline/fenced code, links, blockquotes, ordered/unordered lists, horizontal rules, and escapes
- Streaming text is inserted as plain text for low overhead, then parsed once after completion
- Light/dark styling follows the GNOME system preference

## Fedora dependencies

On a current Fedora Workstation installation:

```sh
sudo dnf install \
  gcc meson ninja-build pkgconf-pkg-config \
  gtk4-devel libadwaita-devel libsoup3-devel \
  json-glib-devel libsecret-devel
```

GNOME Workstation already provides a Secret Service. On another desktop, install and configure a compatible provider such as GNOME Keyring.

The minimum versions in Meson (`GTK 4.14`, `libadwaita 1.5`, `libsoup 3.4`, `json-glib 1.8`) are intentionally aimed at current Fedora rather than older distributions.

## Build and run

From this `linux/` directory:

```sh
meson setup build --buildtype=debug
meson compile -C build
./build/src/dkchatebras
```

For a release build:

```sh
meson setup build-release --buildtype=release
meson compile -C build-release
```

Install system-wide:

```sh
sudo meson install -C build-release
```

Or install under your home directory:

```sh
meson setup build-user --buildtype=release --prefix="$HOME/.local"
meson compile -C build-user
meson install -C build-user
```

After a user install, ensure `$HOME/.local/bin` is on `PATH`. The desktop file and icon are installed below `$HOME/.local/share`.

## API key

1. Launch DKChatebras.
2. Select the key button in the header bar.
3. Paste a Cerebras API key and select **Save Key**.

The key is stored under the Secret Service schema `io.github.dkchatebras.DKChatebras.ApiKey`. It is retrieved immediately before a request and sent only in the authorization header to `api.cerebras.ai`. Network requests go directly from the app to Cerebras; no project-owned proxy is involved.

To remove the key, use the system **Passwords and Keys** application and delete the “DKChatebras Cerebras API key” entry.

## Local history

History is stored with user-only file permissions under:

```text
~/.local/share/dkchatebras/chats/
├── index.json          # sidebar metadata only
├── <chat-uuid>.json    # one complete chat
└── ...
```

The index contains only each chat's UUID, title, model, and update timestamp. Message arrays stay in per-chat files and are loaded lazily. Deleting a chat removes its file and index record. Chat content remains local but is not encrypted; use normal full-disk/home-directory protection if local confidentiality matters.

## Markdown implementation

`src/dk-markdown.c` uses a native UTF-8-aware line/inline parser and `GtkTextTag`s. During SSE streaming, chunks are appended directly to the assistant `GtkTextBuffer` without reparsing previous content. At stream completion (including a stopped partial response), the accumulated response is parsed once and tags are applied. This avoids quadratic redraw/parser behavior and keeps all rendering native.

The parser intentionally supports common chat output rather than the entire CommonMark/GFM specification. It can later be replaced by cmark-gfm token walking without changing the UI or streaming architecture; no HTML renderer is used.

## Build an AppImage

AppImages must be produced on Linux. Build on the oldest **current Fedora** environment you intend to support, ideally in a clean Fedora container or VM. This repository's Windows host is not suitable for compiling or packaging the Linux binary.

Install packaging requirements:

```sh
sudo dnf install \
  gcc meson ninja-build pkgconf-pkg-config curl file patchelf \
  gtk4-devel libadwaita-devel libsoup3-devel \
  json-glib-devel libsecret-devel librsvg2-devel \
  gobject-introspection-devel
```

FUSE is optional because the downloaded linuxdeploy AppImage can also be extracted manually, but for normal execution install:

```sh
sudo dnf install fuse-libs
```

Run:

```sh
chmod +x packaging/build-appimage.sh
./packaging/build-appimage.sh
```

The script:

1. Downloads the continuous `linuxdeploy` AppImage and the GTK plugin script into `.appimage-tools/`.
2. Creates a clean release build.
3. Installs into `AppDir/` with `/usr` as the internal prefix.
4. Uses `linuxdeploy` plus its GTK plugin to collect runtime libraries and emit `DKChatebras-<architecture>.AppImage`.

The first run requires network access to GitHub. Review downloaded tooling before using it in a release pipeline. AppImage bundling is architecture-specific and does not bundle the host Secret Service daemon; the target desktop must provide the `org.freedesktop.secrets` D-Bus service. The Cerebras API also requires internet access and may have account usage costs/rate limits.

If FUSE is unavailable, extract the downloaded linuxdeploy AppImage and point the script's `LINUXDEPLOY` variable at its `AppRun`, or invoke it with `--appimage-extract-and-run`.

## Validation on Fedora

Recommended checks after building:

```sh
meson test -C build --print-errorlogs
appstreamcli validate --no-net data/io.github.dkchatebras.DKChatebras.metainfo.xml
desktop-file-validate data/io.github.dkchatebras.DKChatebras.desktop
```

Then test key storage, a complete streamed response, stopping midway, narrow-window sidebar behavior, model switching, relaunch/history loading, and deletion.

## License

GPL-3.0-or-later. See `COPYING`.
