#include <windows.h>
#include <commdlg.h>
#include <richedit.h>
#include <iostream>
#include <string>
#include "markdown.h"

namespace {
bool HasEffect(HWND edit, const wchar_t* text, DWORD effect) {
    FINDTEXTEXW find{};
    find.chrg = {0, -1};
    find.lpstrText = const_cast<wchar_t*>(text);
    if (SendMessageW(edit, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find)) < 0) return false;
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    SendMessageW(edit, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    return (format.dwMask & effect) != 0 && (format.dwEffects & effect) != 0;
}
}

int main() {
    if (!LoadLibraryW(L"Msftedit.dll")) return 1;
    HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE,
                                0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!edit) return 2;

    const std::wstring markdown =
        L"plain first\n\nplain after breaks\n\n**bold only** and normal text\n\n"
        L"| Name | Value |\n| :--- | ---: |\n| Alpha | **Yes** |\n| Beta | No |";
    RenderMarkdown(edit, markdown, false);

    const int length = GetWindowTextLengthW(edit);
    std::wstring rendered(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, rendered.data(), length + 1);
    rendered.resize(static_cast<size_t>(length));

    const bool syntaxRemoved = rendered.find(L"**") == std::wstring::npos &&
                               rendered.find(L'|') == std::wstring::npos &&
                               rendered.find(L":---") == std::wstring::npos;
    const bool stylesCorrect = HasEffect(edit, L"bold only", CFE_BOLD) &&
                               HasEffect(edit, L"Name", CFE_BOLD) &&
                               HasEffect(edit, L"Yes", CFE_BOLD) &&
                               !HasEffect(edit, L"plain after breaks", CFE_BOLD) &&
                               !HasEffect(edit, L"normal text", CFE_BOLD) &&
                               !HasEffect(edit, L"Alpha", CFE_BOLD) &&
                               !HasEffect(edit, L"No", CFE_BOLD);

    DestroyWindow(edit);
    if (!syntaxRemoved || !stylesCorrect) {
        std::wcerr << L"Markdown regression failed\nRendered:\n" << rendered << L'\n';
        return 3;
    }
    std::cout << "Windows RichEdit Markdown regression passed\n";
    return 0;
}
