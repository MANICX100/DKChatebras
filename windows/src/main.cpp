#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <shellapi.h>
#include <windowsx.h>
#include <atomic>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "storage.h"
#include "network.h"
#include "markdown.h"

namespace {
constexpr wchar_t kAppClass[] = L"DKChatebras.MainWindow";
constexpr wchar_t kSettingsClass[] = L"DKChatebras.SettingsWindow";
constexpr UINT WM_STREAM_CHUNK = WM_APP + 1;
constexpr UINT WM_STREAM_DONE = WM_APP + 2;
constexpr UINT WM_SAVE_ERROR = WM_APP + 3;
enum : int { ID_CHAT = 100, ID_INPUT, ID_SEND, ID_STOP, ID_NEW, ID_DELETE, ID_SETTINGS, ID_MODEL,
    ID_KEY = 200, ID_SAVE_KEY, ID_CANCEL_KEY };

class App;
App* g_app = nullptr;
WNDPROC g_inputProc = nullptr;

template <typename T>
bool PostOwnedMessage(HWND target, UINT message, std::unique_ptr<T> value) {
    T* raw = value.release();
    if (PostMessageW(target, message, 0, reinterpret_cast<LPARAM>(raw))) return true;
    delete raw;
    return false;
}

bool IsDarkPreference() {
    HKEY key = nullptr;
    DWORD value = 1, size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(key);
    }
    return value == 0;
}

void EnableModernFrame(HWND window, bool dark) {
    const BOOL enabled = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(window, 19, &enabled, sizeof(enabled));
    const DWORD rounded = 2;
    DwmSetWindowAttribute(window, 33, &rounded, sizeof(rounded));
}

int MaxInt(int left, int right) { return left > right ? left : right; }

void FillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}

void DrawTextSimple(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
    SelectObject(dc, old);
}

class SettingsDialog {
public:
    SettingsDialog(HWND owner, bool dark, const std::wstring& current) : owner_(owner), dark_(dark), value_(current) {}
    bool Show(HINSTANCE instance) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc; wc.hInstance = instance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HBRUSH classBrush = CreateSolidBrush(dark_ ? RGB(30, 30, 34) : RGB(248, 248, 250));
        wc.hbrBackground = classBrush; wc.lpszClassName = kSettingsClass;
        if (!RegisterClassExW(&wc)) {
            const DWORD registrationError = GetLastError();
            DeleteObject(classBrush);
            if (registrationError != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        RECT ownerRect{}; GetWindowRect(owner_, &ownerRect);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass, L"DKChatebras settings",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, ownerRect.left + 100, ownerRect.top + 100, 500, 245,
            owner_, nullptr, instance, this);
        if (!window_) return false;
        EnableWindow(owner_, FALSE); ShowWindow(window_, SW_SHOW); UpdateWindow(window_);
        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        EnableWindow(owner_, TRUE); SetForegroundWindow(owner_);
        return accepted_;
    }
    const std::wstring& Value() const { return value_; }
private:
    HWND owner_ = nullptr, window_ = nullptr, edit_ = nullptr;
    bool dark_ = false, accepted_ = false;
    std::wstring value_;
    HFONT font_ = nullptr;
    HBRUSH editBrush_ = nullptr;

    static LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = reinterpret_cast<SettingsDialog*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = window;
        }
        return self ? self->Handle(msg, wParam, lParam) : DefWindowProcW(window, msg, wParam, lParam);
    }
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            editBrush_ = CreateSolidBrush(dark_ ? RGB(45, 45, 50) : RGB(255, 255, 255));
            CreateWindowW(L"STATIC", L"Cerebras API key", WS_CHILD | WS_VISIBLE, 24, 22, 430, 24, window_, nullptr, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Stored for your Windows account with DPAPI.", WS_CHILD | WS_VISIBLE, 24, 48, 430, 22, window_, nullptr, nullptr, nullptr);
            edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value_.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                24, 78, 434, 34, window_, reinterpret_cast<HMENU>(ID_KEY), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 276, 135, 86, 34,
                window_, reinterpret_cast<HMENU>(ID_SAVE_KEY), nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 372, 135, 86, 34,
                window_, reinterpret_cast<HMENU>(ID_CANCEL_KEY), nullptr, nullptr);
            EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL { SendMessageW(child, WM_SETFONT, font, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font_));
            SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"csk-..."));
            EnableModernFrame(window_, dark_); SetFocus(edit_); return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_SAVE_KEY) {
                int length = GetWindowTextLengthW(edit_);
                value_.resize(static_cast<size_t>(length) + 1);
                GetWindowTextW(edit_, &value_[0], length + 1);
                value_.resize(static_cast<size_t>(length));
                while (!value_.empty() && iswspace(value_.back())) value_.pop_back();
                size_t first = value_.find_first_not_of(L" \t\r\n"); if (first != std::wstring::npos) value_.erase(0, first);
                if (value_.empty()) { MessageBoxW(window_, L"Enter a Cerebras API key.", L"DKChatebras", MB_ICONWARNING); return 0; }
                accepted_ = true; DestroyWindow(window_); return 0;
            }
            if (LOWORD(wParam) == ID_CANCEL_KEY) { DestroyWindow(window_); return 0; }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam); SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dark_ ? RGB(230, 230, 234) : RGB(35, 35, 40));
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, dark_ ? RGB(240, 240, 243) : RGB(25, 25, 30));
            SetBkColor(dc, dark_ ? RGB(45, 45, 50) : RGB(255, 255, 255)); return reinterpret_cast<LRESULT>(editBrush_);
        }
        case WM_CLOSE: DestroyWindow(window_); return 0;
        case WM_DESTROY: DeleteObject(font_); DeleteObject(editBrush_); window_ = nullptr; return 0;
        }
        return DefWindowProcW(window_, msg, wParam, lParam);
    }
};

class App {
public:
    int Run(HINSTANCE instance, int show) {
        instance_ = instance; dark_ = IsDarkPreference();
        LoadLibraryW(L"Msftedit.dll");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
        std::wstring startupError;
        if (EnsureAppDataDirectory(startupError)) LoadApiKey(apiKey_, startupError);

        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc; wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = kAppClass;
        if (!RegisterClassExW(&wc)) return 1;
        window_ = CreateWindowExW(0, kAppClass, L"DKChatebras", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 780, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        EnableModernFrame(window_, dark_); ShowWindow(window_, show); UpdateWindow(window_); FocusInput();
        if (!startupError.empty()) MessageBoxW(window_, startupError.c_str(), L"DKChatebras storage", MB_ICONERROR);
        if (apiKey_.empty()) PostMessageW(window_, WM_COMMAND, ID_SETTINGS, 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        return static_cast<int>(message.wParam);
    }

    void Send() {
        if (streaming_) return;
        int length = GetWindowTextLengthW(input_);
        if (length <= 0) return;
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(input_, &text[0], length + 1);
        text.resize(static_cast<size_t>(length));
        if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
        if (apiKey_.empty()) { OpenSettings(); if (apiKey_.empty()) return; }
        if (currentId_.empty()) currentId_ = NewConversationId();
        messages_.push_back({L"user", text});
        messages_.push_back({L"assistant", L""});
        SetWindowTextW(input_, L"");
        streamPrefix_ = BuildTranscript(false);
        SetRichEditPlainText(chat_, streamPrefix_);
        SaveConversationAsync();
        streaming_ = true; stopRequested_ = false; SetUiStreaming(true);
        wchar_t modelText[64] = {}; GetWindowTextW(model_, modelText, 64);
        std::vector<ChatMessage> requestMessages = messages_; requestMessages.pop_back();
        std::wstring key = apiKey_, model = modelText;
        HWND target = window_;
        worker_ = std::thread([target, key, model, requestMessages, this] {
            auto callback = [target](const std::wstring& chunk) {
                PostOwnedMessage(target, WM_STREAM_CHUNK, std::make_unique<std::wstring>(chunk));
            };
            auto result = std::make_unique<StreamResult>(StreamChatCompletion(
                key, model, requestMessages, stopRequested_, activeRequest_, callback));
            PostOwnedMessage(target, WM_STREAM_DONE, std::move(result));
        });
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr, chat_ = nullptr, input_ = nullptr, send_ = nullptr, stop_ = nullptr;
    HWND new_ = nullptr, delete_ = nullptr, settings_ = nullptr, model_ = nullptr;
    HFONT uiFont_ = nullptr, titleFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr, chatBrush_ = nullptr;
    bool dark_ = false, streaming_ = false, inputFocused_ = false;
    RECT inputOuter_{};
    UINT dpi_ = 96;
    std::atomic_bool stopRequested_{false};
    std::atomic<void*> activeRequest_{nullptr};
    std::thread worker_;
    std::thread saveWorker_;
    std::wstring apiKey_, currentId_, streamPrefix_;
    std::vector<ChatMessage> messages_;
    std::vector<ConversationSummary> summaries_;
    int sidebarScroll_ = 0;

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    COLORREF Background() const { return dark_ ? RGB(17, 17, 20) : RGB(243, 244, 247); }
    COLORREF Surface() const { return dark_ ? RGB(24, 24, 27) : RGB(250, 250, 252); }
    COLORREF Sidebar() const { return dark_ ? RGB(28, 28, 32) : RGB(232, 234, 239); }
    COLORREF Text() const { return dark_ ? RGB(235, 235, 238) : RGB(30, 32, 38); }
    COLORREF Muted() const { return dark_ ? RGB(158, 158, 168) : RGB(96, 101, 112); }
    COLORREF Accent() const { return dark_ ? RGB(70, 104, 214) : RGB(53, 89, 210); }
    COLORREF InputBackground() const {
        if (!inputFocused_) return Surface();
        return dark_ ? RGB(31, 38, 55) : RGB(240, 245, 255);
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = window; g_app = self;
        }
        return self ? self->Handle(msg, wParam, lParam) : DefWindowProcW(window, msg, wParam, lParam);
    }

    static LRESULT CALLBACK InputProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN && wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            if (g_app) g_app->Send(); return 0;
        }
        return CallWindowProcW(g_inputProc, window, msg, wParam, lParam);
    }

    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: CreateControls(); RefreshSummaries(); NewChat(); return 0;
        case WM_SIZE: Layout(LOWORD(lParam), HIWORD(lParam)); InvalidateRect(window_, nullptr, TRUE); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam); const RECT* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RecreateFonts(); return 0;
        }
        case WM_PAINT: Paint(); return 0;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
        case WM_LBUTTONDOWN: SidebarClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEWHEEL:
            { POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(window_, &cursor);
            if (cursor.x < S(270)) { sidebarScroll_ = MaxInt(0, sidebarScroll_ - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * S(56)); InvalidateRect(window_, nullptr, FALSE); return 0; } }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_INPUT &&
                (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)) {
                inputFocused_ = HIWORD(wParam) == EN_SETFOCUS;
                RecreateInputBrush();
                InvalidateRect(input_, nullptr, TRUE);
                InvalidateRect(window_, &inputOuter_, FALSE);
                return 0;
            }
            switch (LOWORD(wParam)) {
            case ID_SEND: Send(); return 0;
            case ID_STOP: RequestStop(); EnableWindow(stop_, FALSE); return 0;
            case ID_NEW: if (!streaming_) NewChat(); return 0;
            case ID_DELETE: if (!streaming_) DeleteCurrent(); return 0;
            case ID_SETTINGS: OpenSettings(); return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == ID_CHAT && header->code == EN_LINK) OpenLink(*reinterpret_cast<ENLINK*>(lParam));
            break;
        }
        case WM_SAVE_ERROR: {
            std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
            MessageBoxW(window_, error->c_str(), L"DKChatebras storage", MB_ICONERROR);
            return 0;
        }
        case WM_STREAM_CHUNK: {
            std::unique_ptr<std::wstring> chunk(reinterpret_cast<std::wstring*>(lParam));
            if (streaming_ && !messages_.empty()) { messages_.back().content += *chunk; AppendRichEditPlainText(chat_, *chunk); }
            return 0;
        }
        case WM_STREAM_DONE: {
            std::unique_ptr<StreamResult> result(reinterpret_cast<StreamResult*>(lParam));
            if (worker_.joinable()) worker_.join();
            streaming_ = false; SetUiStreaming(false);
            if (!result->ok && !result->stopped) {
                if (!messages_.empty() && messages_.back().content.empty()) messages_.pop_back();
                MessageBoxW(window_, result->error.c_str(), L"DKChatebras request", MB_ICONERROR);
            } else if (result->stopped && !messages_.empty() && messages_.back().content.empty()) messages_.pop_back();
            if (!currentId_.empty()) SaveConversationAsync();
            RenderConversation(); RefreshSummaries(); FocusInput(); return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, Text());
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const bool isInput = reinterpret_cast<HWND>(lParam) == input_;
            SetTextColor(dc, Text()); SetBkColor(dc, isInput ? InputBackground() : Surface());
            return reinterpret_cast<LRESULT>(isInput ? inputBrush_ : chatBrush_);
        }
        case WM_CLOSE:
            RequestStop();
            if (worker_.joinable()) worker_.join();
            if (saveWorker_.joinable()) saveWorker_.join();
            DiscardPendingStreamMessages();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY: DeleteObject(uiFont_); DeleteObject(titleFont_); DeleteObject(inputBrush_); DeleteObject(chatBrush_); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, msg, wParam, lParam);
    }

    void SaveConversationAsync() {
        if (saveWorker_.joinable()) saveWorker_.join();
        HWND target = window_;
        saveWorker_ = std::thread([target, id = currentId_, snapshot = messages_] {
            std::wstring error;
            if (!SaveConversation(id, snapshot, error))
                PostOwnedMessage(target, WM_SAVE_ERROR, std::make_unique<std::wstring>(error));
        });
    }

    void RequestStop() {
        stopRequested_ = true;
        CancelStreamRequest(activeRequest_);
    }

    void DiscardPendingStreamMessages() {
        MSG message{};
        while (PeekMessageW(&message, window_, WM_STREAM_CHUNK, WM_SAVE_ERROR, PM_REMOVE)) {
            if (message.message == WM_STREAM_CHUNK || message.message == WM_SAVE_ERROR)
                delete reinterpret_cast<std::wstring*>(message.lParam);
            else if (message.message == WM_STREAM_DONE)
                delete reinterpret_cast<StreamResult*>(message.lParam);
        }
    }

    void CreateControls() {
        HDC dc = GetDC(window_); dpi_ = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(window_, dc);
        RecreateFonts();
        inputBrush_ = CreateSolidBrush(InputBackground()); chatBrush_ = CreateSolidBrush(Surface());
        chat_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(ID_CHAT), instance_, nullptr);
        SendMessageW(chat_, EM_SETBKGNDCOLOR, 0, Surface());
        SendMessageW(chat_, EM_SETEVENTMASK, 0, SendMessageW(chat_, EM_GETEVENTMASK, 0, 0) | ENM_LINK);
        SendMessageW(chat_, EM_AUTOURLDETECT, TRUE, 0);
        input_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(ID_INPUT), instance_, nullptr);
        SendMessageW(input_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Message DKChatebras…  (Shift+Enter for a new line)"));
        g_inputProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
        send_ = MakeButton(L"Send", ID_SEND); stop_ = MakeButton(L"Stop", ID_STOP);
        new_ = MakeButton(L"+  New chat", ID_NEW); delete_ = MakeButton(L"Delete", ID_DELETE); settings_ = MakeButton(L"Settings", ID_SETTINGS);
        model_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(ID_MODEL), instance_, nullptr);
        SendMessageW(model_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"gpt-oss-120b"));
        SendMessageW(model_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"gemma-4-31b"));
        SendMessageW(model_, CB_SETCURSEL, 0, 0);
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL { SendMessageW(child, WM_SETFONT, font, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(uiFont_));
        SetUiStreaming(false);
    }

    HWND MakeButton(const wchar_t* text, int id) {
        return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    void RecreateFonts() {
        DeleteObject(uiFont_); DeleteObject(titleFont_);
        uiFont_ = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        titleFont_ = CreateFontW(-S(23), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (window_) EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL { SendMessageW(child, WM_SETFONT, font, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(uiFont_));
    }

    void Layout(int width, int height) {
        int sidebar = S(270), pad = S(22), header = S(76), composer = S(142), button = S(84), gap = S(10);
        MoveWindow(model_, width - S(194), S(20), S(168), S(220), TRUE);
        MoveWindow(chat_, sidebar + pad, header, MaxInt(10, width - sidebar - pad * 2), MaxInt(10, height - header - composer), TRUE);
        inputOuter_ = {sidebar + pad, height - S(116),
            sidebar + pad + MaxInt(10, width - sidebar - pad * 2 - button - gap), height - S(28)};
        const int inputBorder = S(2);
        MoveWindow(input_, inputOuter_.left + inputBorder, inputOuter_.top + inputBorder,
            MaxInt(6, inputOuter_.right - inputOuter_.left - inputBorder * 2),
            MaxInt(6, inputOuter_.bottom - inputOuter_.top - inputBorder * 2), TRUE);
        MoveWindow(send_, width - pad - button, height - S(116), button, S(42), TRUE);
        MoveWindow(stop_, width - pad - button, height - S(66), button, S(38), TRUE);
        MoveWindow(new_, S(18), S(72), S(234), S(42), TRUE);
        MoveWindow(delete_, S(18), height - S(104), S(108), S(38), TRUE);
        MoveWindow(settings_, S(134), height - S(104), S(118), S(38), TRUE);
    }

    void Paint() {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window_, &ps);
        RECT client{}; GetClientRect(window_, &client);
        HBRUSH background = CreateSolidBrush(Background()); FillRect(dc, &client, background); DeleteObject(background);
        RECT sidebar{0, 0, S(270), client.bottom}; HBRUSH side = CreateSolidBrush(Sidebar()); FillRect(dc, &sidebar, side); DeleteObject(side);
        RECT title{S(22), S(20), S(248), S(58)}; DrawTextSimple(dc, L"DKChatebras", title, titleFont_, Text(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT chatTitle{S(294), S(20), client.right - S(220), S(58)};
        DrawTextSimple(dc, currentId_.empty() ? L"New conversation" : CurrentTitle(), chatTitle, titleFont_, Text(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT historyLabel{S(22), S(130), S(248), S(158)}; DrawTextSimple(dc, L"RECENT", historyLabel, uiFont_, Muted(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int y = S(164) - sidebarScroll_;
        if (inputOuter_.right > inputOuter_.left) {
            FillRoundRect(dc, inputOuter_, S(10), inputFocused_ ? Accent() :
                (dark_ ? RGB(63, 63, 70) : RGB(203, 207, 216)));
        }
        for (const auto& summary : summaries_) {
            RECT item{S(14), y, S(256), y + S(50)};
            if (summary.id == currentId_) FillRoundRect(dc, item, S(12), dark_ ? RGB(48, 48, 56) : RGB(216, 220, 230));
            RECT textRect{S(25), y, S(246), y + S(50)};
            DrawTextSimple(dc, summary.title, textRect, uiFont_, summary.id == currentId_ ? Text() : Muted(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += S(56);
        }
        EndPaint(window_, &ps);
    }

    void DrawButton(const DRAWITEMSTRUCT& item) {
        bool disabled = (item.itemState & ODS_DISABLED) != 0;
        bool pressed = (item.itemState & ODS_SELECTED) != 0;
        COLORREF fill = item.CtlID == ID_SEND ? Accent() : (dark_ ? RGB(48, 48, 54) : RGB(224, 226, 232));
        if (pressed) fill = dark_ ? RGB(60, 65, 78) : RGB(201, 205, 216);
        FillRoundRect(item.hDC, item.rcItem, S(11), fill);
        wchar_t text[80]{}; GetWindowTextW(item.hwndItem, text, 80);
        RECT rect = item.rcItem;
        COLORREF color = disabled ? Muted() : (item.CtlID == ID_SEND ? RGB(255, 255, 255) : Text());
        DrawTextSimple(item.hDC, text, rect, uiFont_, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item.itemState & ODS_FOCUS) { RECT focus = rect; InflateRect(&focus, -S(4), -S(4)); DrawFocusRect(item.hDC, &focus); }
    }

    void SidebarClick(int x, int y) {
        if (x >= S(270) || y < S(164)) return;
        int index = (y - S(164) + sidebarScroll_) / S(56);
        if (index >= 0 && index < static_cast<int>(summaries_.size()) && !streaming_) SelectConversation(summaries_[static_cast<size_t>(index)].id);
    }

    void RefreshSummaries() { summaries_ = ListConversations(); InvalidateRect(window_, nullptr, FALSE); }
    std::wstring CurrentTitle() const {
        for (const auto& summary : summaries_) if (summary.id == currentId_) return summary.title;
        return L"New conversation";
    }
    void NewChat() { currentId_.clear(); messages_.clear(); RenderConversation(); FocusInput(); InvalidateRect(window_, nullptr, FALSE); }
    void SelectConversation(const std::wstring& id) {
        std::vector<ChatMessage> loaded; std::wstring error;
        if (!LoadConversation(id, loaded, error)) { MessageBoxW(window_, error.c_str(), L"DKChatebras", MB_ICONERROR); return; }
        currentId_ = id; messages_ = std::move(loaded); RenderConversation(); FocusInput(); InvalidateRect(window_, nullptr, FALSE);
    }
    void DeleteCurrent() {
        if (currentId_.empty()) { NewChat(); return; }
        if (MessageBoxW(window_, L"Delete this conversation? This cannot be undone.", L"DKChatebras", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        std::wstring error;
        if (!DeleteConversation(currentId_, error)) { MessageBoxW(window_, error.c_str(), L"DKChatebras", MB_ICONERROR); return; }
        NewChat(); RefreshSummaries();
    }

    std::wstring BuildTranscript(bool includeEmptyAssistant = true) const {
        std::wstring transcript;
        for (const auto& message : messages_) {
            if (!includeEmptyAssistant && message.role == L"assistant" && message.content.empty()) {
                transcript += L"## DKChatebras\r\n"; continue;
            }
            transcript += message.role == L"user" ? L"## You\r\n" : L"## DKChatebras\r\n";
            transcript += message.content + L"\r\n\r\n";
        }
        return transcript;
    }
    void RenderConversation() {
        if (messages_.empty()) {
            RenderMarkdown(chat_, L"# Welcome to DKChatebras\r\n\r\nStart a conversation or select one from your history.", dark_);
            return;
        }
        std::vector<MarkdownMessage> transcript;
        transcript.reserve(messages_.size());
        for (const auto& message : messages_) {
            transcript.push_back({message.role == L"user" ? L"You" : L"DKChatebras", message.content});
        }
        RenderMarkdownTranscript(chat_, transcript, dark_);
    }
    void SetUiStreaming(bool active) {
        EnableWindow(send_, !active); EnableWindow(new_, !active); EnableWindow(delete_, !active); EnableWindow(model_, !active);
        EnableWindow(stop_, active); SetWindowTextW(stop_, L"Stop"); InvalidateRect(stop_, nullptr, TRUE);
    }
    void OpenSettings() {
        SettingsDialog dialog(window_, dark_, apiKey_);
        if (dialog.Show(instance_)) {
            std::wstring error;
            if (!SaveApiKey(dialog.Value(), error)) MessageBoxW(window_, error.c_str(), L"DKChatebras", MB_ICONERROR);
            else apiKey_ = dialog.Value();
        }
        FocusInput();
    }
    void RecreateInputBrush() {
        DeleteObject(inputBrush_);
        inputBrush_ = CreateSolidBrush(InputBackground());
    }

    void FocusInput() {
        if (!input_) return;
        SetFocus(input_);
        SendMessageW(input_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(input_, EM_SCROLLCARET, 0, 0);
    }

    void OpenLink(const ENLINK& link) {
        if (link.msg != WM_LBUTTONUP) return;
        TEXTRANGEW range{}; range.chrg = link.chrg;
        std::wstring url(static_cast<size_t>(link.chrg.cpMax - link.chrg.cpMin + 1), L'\0'); range.lpstrText = &url[0];
        SendMessageW(chat_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&range)); url.resize(wcslen(url.c_str()));
        if (url.compare(0, 7, L"http://") == 0 || url.compare(0, 8, L"https://") == 0)
            ShellExecuteW(window_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
};
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    SetProcessDPIAware();
    App app;
    return app.Run(instance, show);
}
