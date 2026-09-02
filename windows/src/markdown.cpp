#include "markdown.h"
#include <cwctype>
#include <richedit.h>
#include <vector>

namespace {
enum class Style {
    Bold, Italic, BoldItalic, Strike, Code, CodeBlock,
    Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
    Link, Quote
};
struct Span { LONG start; LONG length; Style style; };

void AddSpan(std::vector<Span>& spans, size_t start, size_t end, Style style) {
    if (end > start)
        spans.push_back({static_cast<LONG>(start), static_cast<LONG>(end - start), style});
}

void Apply(HWND edit, LONG start, LONG length, DWORD effects, DWORD mask,
           COLORREF color = 0, COLORREF background = 0,
           const wchar_t* face = nullptr, LONG height = 0) {
    CHARRANGE range{start, start + length};
    SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = mask;
    format.dwEffects = effects;
    if (mask & CFM_COLOR) format.crTextColor = color;
    if (mask & CFM_BACKCOLOR) format.crBackColor = background;
    if ((mask & CFM_FACE) && face) wcsncpy_s(format.szFaceName, face, _TRUNCATE);
    if (mask & CFM_SIZE) format.yHeight = height;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
}

size_t FindClosing(const std::wstring& input, const std::wstring& marker, size_t from) {
    size_t position = from;
    while ((position = input.find(marker, position)) != std::wstring::npos) {
        size_t slashes = 0;
        for (size_t i = position; i > 0 && input[i - 1] == L'\\'; --i) ++slashes;
        if ((slashes & 1U) == 0) return position;
        position += marker.size();
    }
    return std::wstring::npos;
}

void ParseInline(const std::wstring& input, std::wstring& output, std::vector<Span>& spans) {
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == L'\\' && i + 1 < input.size()) {
            output.push_back(input[i + 1]);
            i += 2;
            continue;
        }

        if (input[i] == L'`') {
            size_t ticks = 1;
            while (i + ticks < input.size() && input[i + ticks] == L'`') ++ticks;
            const std::wstring marker(ticks, L'`');
            const size_t end = FindClosing(input, marker, i + ticks);
            if (end != std::wstring::npos) {
                const size_t start = output.size();
                output.append(input, i + ticks, end - i - ticks);
                AddSpan(spans, start, output.size(), Style::Code);
                i = end + ticks;
                continue;
            }
        }

        if (i + 1 < input.size() && input[i] == L'!' && input[i + 1] == L'[') {
            const size_t close = input.find(L']', i + 2);
            if (close != std::wstring::npos && close + 1 < input.size() && input[close + 1] == L'(') {
                const size_t end = input.find(L')', close + 2);
                if (end != std::wstring::npos) {
                    output.append(input, i + 2, close - i - 2);
                    i = end + 1;
                    continue;
                }
            }
        }

        if (input[i] == L'[') {
            const size_t close = input.find(L']', i + 1);
            if (close != std::wstring::npos && close + 1 < input.size() && input[close + 1] == L'(') {
                const size_t end = input.find(L')', close + 2);
                if (end != std::wstring::npos) {
                    ParseInline(input.substr(i + 1, close - i - 1), output, spans);
                    const std::wstring url = input.substr(close + 2, end - close - 2);
                    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) {
                        output += L" (";
                        const size_t linkStart = output.size();
                        output += url;
                        AddSpan(spans, linkStart, output.size(), Style::Link);
                        output += L')';
                    }
                    i = end + 1;
                    continue;
                }
            }
        }

        struct Marker { const wchar_t* text; Style style; };
        static constexpr Marker markers[] = {
            {L"***", Style::BoldItalic}, {L"___", Style::BoldItalic},
            {L"**", Style::Bold}, {L"__", Style::Bold},
            {L"~~", Style::Strike}, {L"*", Style::Italic}, {L"_", Style::Italic}
        };
        bool matched = false;
        for (const auto& marker : markers) {
            const std::wstring token(marker.text);
            if (input.compare(i, token.size(), token) != 0) continue;
            const size_t end = FindClosing(input, token, i + token.size());
            if (end == std::wstring::npos || end == i + token.size()) continue;
            const size_t start = output.size();
            ParseInline(input.substr(i + token.size(), end - i - token.size()), output, spans);
            AddSpan(spans, start, output.size(), marker.style);
            i = end + token.size();
            matched = true;
            break;
        }
        if (matched) continue;
        output.push_back(input[i++]);
    }
}

bool IsHorizontalRule(const std::wstring& line) {
    wchar_t marker = 0;
    int count = 0;
    for (wchar_t character : line) {
        if (iswspace(character)) continue;
        if (character != L'-' && character != L'*' && character != L'_') return false;
        if (marker == 0) marker = character;
        if (character != marker) return false;
        ++count;
    }
    return count >= 3;
}

bool StripListPrefix(std::wstring& line) {
    size_t indent = 0;
    while (indent < line.size() && iswspace(line[indent])) ++indent;
    if (indent + 1 < line.size() &&
        (line[indent] == L'-' || line[indent] == L'*' || line[indent] == L'+') &&
        iswspace(line[indent + 1])) {
        line = std::wstring(indent / 2, L' ') + L"• " + line.substr(indent + 2);
        return true;
    }
    size_t digit = indent;
    while (digit < line.size() && iswdigit(line[digit])) ++digit;
    if (digit > indent && digit + 1 < line.size() &&
        (line[digit] == L'.' || line[digit] == L')') && iswspace(line[digit + 1])) {
        line = std::wstring(indent / 2, L' ') + line.substr(indent, digit - indent + 1) + L" " + line.substr(digit + 2);
        return true;
    }
    return false;
}

std::wstring Trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t");
    return value.substr(first, last - first + 1);
}

bool IsTableSeparator(const std::wstring& line) {
    std::wstring value = Trim(line);
    if (!value.empty() && value.front() == L'|') value.erase(value.begin());
    if (!value.empty() && value.back() == L'|') value.pop_back();
    size_t start = 0;
    int columns = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L'|', start);
        std::wstring cell = Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!cell.empty() && cell.front() == L':') cell.erase(cell.begin());
        if (!cell.empty() && cell.back() == L':') cell.pop_back();
        cell = Trim(cell);
        if (cell.size() < 3 || cell.find_first_not_of(L'-') != std::wstring::npos) return false;
        ++columns;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return columns > 0;
}

void ParseTableRow(const std::wstring& line, std::wstring& output, std::vector<Span>& spans, bool header) {
    std::wstring value = Trim(line);
    if (!value.empty() && value.front() == L'|') value.erase(value.begin());
    if (!value.empty() && value.back() == L'|') value.pop_back();
    const size_t rowStart = output.size();
    size_t start = 0;
    bool first = true;
    while (start <= value.size()) {
        const size_t end = value.find(L'|', start);
        if (!first) output += L'\t';
        ParseInline(Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start)), output, spans);
        first = false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (header) AddSpan(spans, rowStart, output.size(), Style::Bold);
}

Style HeadingStyle(size_t level) {
    switch (level) {
    case 1: return Style::Heading1;
    case 2: return Style::Heading2;
    case 3: return Style::Heading3;
    case 4: return Style::Heading4;
    case 5: return Style::Heading5;
    default: return Style::Heading6;
    }
}
}

void SetRichEditPlainText(HWND richEdit, const std::wstring& text) {
    SETTEXTEX set{ST_DEFAULT, 1200};
    SendMessageW(richEdit, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&set), reinterpret_cast<LPARAM>(text.c_str()));
}

void AppendRichEditPlainText(HWND richEdit, const std::wstring& text) {
    CHARRANGE end{-1, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(richEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(richEdit, EM_SCROLLCARET, 0, 0);
}

void RenderMarkdown(HWND richEdit, const std::wstring& markdown, bool darkMode) {
    std::wstring output;
    std::vector<Span> spans;
    bool codeBlock = false;
    bool tableMode = false;
    size_t position = 0;

    while (position <= markdown.size()) {
        size_t end = markdown.find(L'\n', position);
        const bool last = end == std::wstring::npos;
        if (last) end = markdown.size();
        std::wstring line = markdown.substr(position, end - position);
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        std::wstring nextLine;
        if (!last) {
            const size_t nextStart = end + 1;
            size_t nextEnd = markdown.find(L'\n', nextStart);
            if (nextEnd == std::wstring::npos) nextEnd = markdown.size();
            nextLine = markdown.substr(nextStart, nextEnd - nextStart);
            if (!nextLine.empty() && nextLine.back() == L'\r') nextLine.pop_back();
        }
        const bool beginsTable = line.find(L'|') != std::wstring::npos && IsTableSeparator(nextLine);
        const bool tableRow = line.find(L'|') != std::wstring::npos && (tableMode || beginsTable);
        const std::wstring trimmed = Trim(line);
        if (trimmed.rfind(L"```", 0) == 0 || trimmed.rfind(L"~~~", 0) == 0) {
            codeBlock = !codeBlock;
        } else if (codeBlock) {
            const size_t start = output.size();
            output += line;
            AddSpan(spans, start, output.size(), Style::CodeBlock);
            if (!last) output += L'\r';
        } else if (tableMode && IsTableSeparator(line)) {
            // The GFM separator row controls table structure but is not displayed.
        } else if (tableRow) {
            ParseTableRow(line, output, spans, beginsTable);
            if (!last) output += L'\r';
            tableMode = true;
        } else if (IsHorizontalRule(line)) {
            tableMode = false;
            output += L"────────────────────────────────────────";
            if (!last) output += L'\r';
        } else {
            tableMode = false;
            size_t heading = 0;
            while (heading < line.size() && heading < 6 && line[heading] == L'#') ++heading;
            if (heading == 0 || heading >= line.size() || !iswspace(line[heading])) heading = 0;

            bool quote = false;
            size_t quotePrefix = 0;
            while (quotePrefix + 1 < line.size() && line[quotePrefix] == L'>' && iswspace(line[quotePrefix + 1])) {
                quote = true;
                quotePrefix += 2;
            }
            if (quote) line = L"│ " + line.substr(quotePrefix);
            StripListPrefix(line);

            const size_t start = output.size();
            ParseInline(heading ? line.substr(heading + 1) : line, output, spans);
            if (heading) AddSpan(spans, start, output.size(), HeadingStyle(heading));
            if (quote) AddSpan(spans, start, output.size(), Style::Quote);
            if (!last) output += L'\r';
        }
        if (last) break;
        position = end + 1;
    }

    SetRichEditPlainText(richEdit, output);
    CHARRANGE all{0, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all));
    CHARFORMAT2W base{};
    base.cbSize = sizeof(base);
    base.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BACKCOLOR |
                  CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_LINK;
    base.dwEffects = 0;
    base.yHeight = 210;
    base.crTextColor = darkMode ? RGB(229, 231, 235) : RGB(31, 41, 55);
    base.crBackColor = darkMode ? RGB(24, 24, 27) : RGB(250, 250, 252);
    wcscpy_s(base.szFaceName, L"Segoe UI");
    SendMessageW(richEdit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

    const COLORREF codeText = darkMode ? RGB(232, 226, 255) : RGB(43, 35, 75);
    const COLORREF codeBackground = darkMode ? RGB(38, 38, 45) : RGB(235, 231, 242);
    for (const auto& span : spans) {
        switch (span.style) {
        case Style::Bold: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD); break;
        case Style::Italic: Apply(richEdit, span.start, span.length, CFE_ITALIC, CFM_ITALIC); break;
        case Style::BoldItalic: Apply(richEdit, span.start, span.length, CFE_BOLD | CFE_ITALIC, CFM_BOLD | CFM_ITALIC); break;
        case Style::Strike: Apply(richEdit, span.start, span.length, CFE_STRIKEOUT, CFM_STRIKEOUT); break;
        case Style::Code:
        case Style::CodeBlock: Apply(richEdit, span.start, span.length, 0, CFM_FACE | CFM_COLOR | CFM_BACKCOLOR, codeText, codeBackground, L"Consolas"); break;
        case Style::Heading1: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 390); break;
        case Style::Heading2: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 330); break;
        case Style::Heading3: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 285); break;
        case Style::Heading4: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 250); break;
        case Style::Heading5: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE, 0, 0, nullptr, 225); break;
        case Style::Heading6: Apply(richEdit, span.start, span.length, CFE_BOLD, CFM_BOLD | CFM_SIZE | CFM_COLOR, darkMode ? RGB(175, 178, 190) : RGB(78, 84, 98), 0, nullptr, 210); break;
        case Style::Link: Apply(richEdit, span.start, span.length, CFE_LINK, CFM_LINK | CFM_COLOR, darkMode ? RGB(130, 160, 255) : RGB(37, 99, 235)); break;
        case Style::Quote: Apply(richEdit, span.start, span.length, CFE_ITALIC, CFM_ITALIC | CFM_COLOR, darkMode ? RGB(180, 184, 198) : RGB(86, 91, 104)); break;
        }
    }
    CHARRANGE finish{-1, -1};
    SendMessageW(richEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&finish));
    SendMessageW(richEdit, EM_SCROLLCARET, 0, 0);
}

void RenderMarkdownTranscript(HWND richEdit, const std::vector<MarkdownMessage>& messages, bool darkMode) {
    std::wstring markdown;
    for (const auto& message : messages) {
        markdown += L"## ";
        markdown += message.author;
        markdown += L"\r\n\r\n";
        markdown += message.content;
        markdown += L"\r\n\r\n";
    }
    RenderMarkdown(richEdit, markdown, darkMode);
}
