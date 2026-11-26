# DKChatebras for Linux

DKChatebras for Linux is a fast, minimal, dependency-light native desktop client written in C17. It talks directly to X11 (with MIT-SHM), FreeType2, Fontconfig, and lazy-loaded system OpenSSL (`libssl.so.4` / `libssl.so.3` / `libssl.so.1.1`) — with **no Electron, WebView, GTK, Qt, GLib, libsoup, json-glib, or libsecret**.

The stripped release executable is **~176 KiB** and links directly to only **6 system libraries** (`libX11`, `libXext`, `libfreetype`, `libfontconfig`, `libm`, `libc`).

## Features

- **Minimal & fast native UI**: Direct X11 + MIT-SHM ARGB32 software rendering with analytical anti-aliased rounded geometry, FreeType2 glyph caching with automatic Fontconfig fallback faces, viewport culling, and 60 fps streaming paint pacing
- **Responsive layout**: Collapsible sidebar that automatically switches to an overlay drawer below `740px` width, top-bar model selector (`gpt-oss-120b`, `gemma-4-31b`), API key modal, empty-state starter prompts, toast notifications, and full X11 `CLIPBOARD` copy/cut/paste
- **Rich native Markdown, GFM Tables & LaTeX**: Zero-dependency single-pass parser supporting headings 1–6, nested bold/italic/strikethrough, inline code pills, clickable links (`xdg-open`), coral-bordered blockquotes, ordered/unordered lists, horizontal rules, fenced code blocks with language headers, bordered auto-sized GFM tables with `<br>` support, balanced-brace LaTeX math normalization (`\frac`, `\underbrace`, Greek & math symbols), and keycap/emoji glyph normalization
- **Direct streaming HTTPS**: Non-blocking POSIX TCP + lazy `dlopen` OpenSSL with full `SSL_VERIFY_PEER` certificate and hostname verification, HTTP/1.1 chunked/SSE streaming, and instant pipe-signaled cancellation
- **Lazy local history**: 100% compatible with previous versions (`~/.local/share/dkchatebras/chats/index.json` + `<uuid>.json`), atomic `0600` writes on a background writer thread so disk I/O never stalls the UI
- **Credential storage**: Checks `CEREBRAS_API_KEY` env var → `secret-tool` (GNOME Keyring / KWallet Secret Service, compatible with existing `service=cerebras-api` entries) → `0600` fallback file at `$XDG_CONFIG_HOME/dkchatebras/api_key`

## Keyboard shortcuts

- `Enter` — Send message (`Shift+Enter` for newline)
- `Ctrl+N` — New chat
- `Ctrl+B` — Toggle sidebar
- `Ctrl+K` — Open API key modal
- `Ctrl+C` / `Ctrl+X` / `Ctrl+V` / `Ctrl+A` — Clipboard and selection in composer or message selection
- `PageUp` / `PageDown` — Scroll conversation
- `Escape` — Close modal/dropdown, clear selection, or stop active generation
- `Ctrl+Q` — Quit

## Benchmarks vs previous GTK4/libadwaita build (`v3`)

Measured on Fedora 45 (`x86_64`, live X11 `:0`), same isolated `XDG_DATA_HOME` fixture:

| Metric | Old GTK4/libadwaita (`v3`) | New Native C17/X11 (`v4`) | Improvement |
| --- | ---: | ---: | ---: |
| Release artifact size | 32.19 MiB (AppImage) | **176 KiB** binary / **~94 KiB** `.tar.gz` | **188× – 350× smaller** |
| Direct `NEEDED` shared libs | 9 (GTK4/Adwaita/Soup/JSON/Secret) | **6** (`X11`, `Xext`, `freetype`, `fontconfig`, `m`, `c`) | No toolkit stack |
| Total `ldd` runtime libs | 119 | **20** | **6.0× fewer** |
| Startup to mapped window (median, 8 runs) | 123.8 ms | **35.6 ms** | **3.5× faster** |
| Idle memory (RSS / PSS / PrivDirty) | 117.0 / 50.2 / 29.3 MiB | **15.6 / 4.55 / 3.27 MiB** | **7.5×–11× lower** |
| 50-message chat loaded (RSS / PSS / PrivDirty) | 192.3 / 121.1 / 99.2 MiB | **28.3 / 11.2 / 3.62 MiB** | **6.8×–27× lower** |
| Idle threads | 22–24 | **2** | **11× fewer** |
| Markdown parse (2,000 iters, rich doc) | 46.29 µs/iter (20.5 MB/s) | **5.94 µs/iter (160.0 MB/s)** | **7.8× faster** |
| Markdown parse + full wrap/table layout | 46.29 µs/iter (parse only) | **23.56 µs/iter (40.4 MB/s)** | **2.0× faster** (incl. layout) |

## Build dependencies

Only a C17 compiler, `make`, `pkg-config`, and development headers for X11/Xext/FreeType2/Fontconfig are needed (plus system `libssl` at runtime for HTTPS; `secret-tool` is optional for keyring storage):

**Fedora:**
```sh
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel freetype-devel fontconfig-devel
```

**Debian / Ubuntu:**
```sh
sudo apt install gcc make pkg-config libx11-dev libxext-dev libfreetype-dev libfontconfig-dev
```

**Arch Linux:**
```sh
sudo pacman -S gcc make pkgconf libx11 libxext freetype2 fontconfig
```

## Build, test, and install

From this `linux/` directory:

```sh
make release          # builds stripped build/dkchatebras (~176 KiB)
make test             # runs 71 Markdown, LaTeX, GFM table, JSON, and store assertions
./build/dkchatebras
```

Debug build (add `SANITIZE=1` for ASan + UBSan):

```sh
make debug
make CC=clang test-asan
```

Install system-wide or per-user:

```sh
sudo make install                      # installs under /usr/local
make install PREFIX="$HOME/.local"     # installs under ~/.local
```

Build release distribution artifacts (`build/dist/DKChatebras-<ver>-linux-<arch>` and `.tar.gz`):

```sh
./packaging/build-release.sh v4
```

## Validation

```sh
make test
desktop-file-validate data/io.github.dkchatebras.DKChatebras.desktop
appstreamcli validate --no-net data/io.github.dkchatebras.DKChatebras.metainfo.xml
```

## License

GPL-3.0-or-later. See `COPYING`.
