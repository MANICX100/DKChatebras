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
        L"| Name | Value |\n| :--- | ---: |\n| Alpha | **Yes** |\n| Beta | risk one<br>risk two |\n\n"
        L"Break-even: \\(\\frac{149}{50}=2.98\\) months\n\n"
        L"\\[ \\underbrace{C_{r}\\times N}_{\\text{total rent}} \\;<\\; \\underbrace{P - R}_{\\text{net loss}} \\]\n\n"
        L"Path stays: C:\\Users\\Dan and code stays: `a \\times b`\n\n"
        L"\u0031\uFE0F\u20E3 First keycap and \u0032\uFE0F\u20E3 second";
    RenderMarkdown(edit, markdown, false);

    const int length = GetWindowTextLengthW(edit);
    std::wstring rendered(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, rendered.data(), length + 1);
    rendered.resize(static_cast<size_t>(length));

    struct Check { const char* label; bool passed; };
    const Check checks[] = {
        {"no ** markers", rendered.find(L"**") == std::wstring::npos},
        {"no pipes", rendered.find(L'|') == std::wstring::npos},
        {"no separator row", rendered.find(L":---") == std::wstring::npos},
        {"no <br>", rendered.find(L"<br") == std::wstring::npos},
        {"no \\frac", rendered.find(L"\\frac") == std::wstring::npos},
        {"no math delimiters", rendered.find(L"\\(") == std::wstring::npos},
        {"fraction converted", rendered.find(L"149\u204450") != std::wstring::npos},
        {"br split first", rendered.find(L"risk one") != std::wstring::npos},
        {"br split second", rendered.find(L"risk two") != std::wstring::npos},
        {"no underbrace", rendered.find(L"underbrace") == std::wstring::npos},
        {"no subscripts", rendered.find(L"_{") == std::wstring::npos},
        {"no ;<;", rendered.find(L";<;") == std::wstring::npos},
        {"expression kept", rendered.find(L"Cr\u00d7 N") != std::wstring::npos},
        {"annotation kept", rendered.find(L"(total rent)") != std::wstring::npos},
        {"windows path kept", rendered.find(L"C:\\Users\\Dan") != std::wstring::npos},
        {"inline code kept", rendered.find(L"a \\times b") != std::wstring::npos},
        {"no variation selector", rendered.find(L'\uFE0F') == std::wstring::npos},
        {"no combining keycap", rendered.find(L'\u20E3') == std::wstring::npos},
        {"keycap one readable", rendered.find(L"1. First keycap") != std::wstring::npos},
        {"keycap two readable", rendered.find(L"2. second") != std::wstring::npos},
        {"bold applied", HasEffect(edit, L"bold only", CFE_BOLD)},
        {"header bold", HasEffect(edit, L"Name", CFE_BOLD)},
        {"bold cell bold", HasEffect(edit, L"Yes", CFE_BOLD)},
        {"paragraph not bold", !HasEffect(edit, L"plain after breaks", CFE_BOLD)},
        {"inline text not bold", !HasEffect(edit, L"normal text", CFE_BOLD)},
        {"body cell not bold", !HasEffect(edit, L"Alpha", CFE_BOLD)},
        {"br cell not bold", !HasEffect(edit, L"risk one", CFE_BOLD)},
    };

    DestroyWindow(edit);
    bool allPassed = true;
    for (const auto& check : checks) {
        if (!check.passed) {
            allPassed = false;
            std::cout << "FAILED: " << check.label << '\n';
        }
    }
    if (!allPassed) {
        HANDLE dump = CreateFileW(L"markdown_test_output.txt", GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const wchar_t bom = 0xFEFF;
            WriteFile(dump, &bom, sizeof(bom), &written, nullptr);
            WriteFile(dump, rendered.c_str(), static_cast<DWORD>(rendered.size() * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(dump);
        }
        return 3;
    }
    std::cout << "Windows RichEdit Markdown regression passed\n";
    return 0;
}
