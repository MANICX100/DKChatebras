# DKChatebras for Windows

DKChatebras is a dependency-free native Win32 desktop client for Cerebras chat completions. It uses C++17, GDI/RichEdit, DPAPI, and WinHTTP—no .NET runtime, browser engine, package manager, or third-party library is required.

## Features

- Streaming Cerebras chat completions over WinHTTP/SSE
- Models: `gpt-oss-120b` and `gemma-4-31b`
- API-key settings stored with user-scoped Windows DPAPI under `%LOCALAPPDATA%\DKChatebras`
- One file per conversation; the sidebar reads compact summary headers and loads full messages only when selected
- New, delete, stop, scrolling, clickable links, and native RichEdit Markdown formatting for headings 1–6, nested bold/italic, strikethrough, inline/fenced code, blockquotes, ordered/unordered lists, GFM tables, links, horizontal rules, and escapes
- The composer receives keyboard focus and an accent highlight by default, after generation, and when switching chats
- Segoe UI and custom-painted rounded surfaces with system dark preference and modern DWM frame attributes where available
- Static MSVC runtime (`/MT`) and a single x64 executable

## Requirements

- Windows 7 SP1 or later
- TLS 1.2 support enabled on Windows 7 (current servicing updates are required for Cerebras HTTPS)
- Visual Studio 2022 Build Tools with the C++ x64 workload
- CMake 3.20 or newer
- A Cerebras API key

The executable targets the Windows 7 (`6.01`) subsystem and avoids statically importing Windows 10/11-only APIs. Visual styling gracefully falls back where newer DWM attributes are unavailable. Actual service connectivity on Windows 7 depends on its TLS/root-certificate updates; that limitation cannot be solved inside an application without bundling a TLS stack.

## Build

From PowerShell:

```powershell
.\build.ps1 -Configuration Release
```

For a clean rebuild:

```powershell
.\build.ps1 -Configuration Release -Clean
```

Output:

```text
build\bin\DKChatebras.exe
```

The script uses `vswhere.exe` to verify MSVC Build Tools, configures an x64 Visual Studio build, and invokes CMake. CMake sets `MSVC_RUNTIME_LIBRARY` to the static multithreaded CRT.

## Data and privacy

- Protected API key: `%LOCALAPPDATA%\DKChatebras\credentials.dat`
- Conversations: `%LOCALAPPDATA%\DKChatebras\conversations\*.chat`
- Requests are sent directly to `https://api.cerebras.ai/v1/chat/completions` using Windows' configured WinHTTP proxy behavior.

DPAPI binds the credential ciphertext to the current Windows user profile. Conversation text itself is stored locally but is not encrypted. Prompt history is sent to Cerebras when requesting a completion.
